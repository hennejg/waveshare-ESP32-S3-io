/* The poll table's values as Matter sensors.
 *
 * A row read from a device on RS-485 that names a Matter kind (app_config.h,
 * MBM_MATTER_*) gets an endpoint of the matching device type at boot:
 * temperature, humidity, pressure and flow sensors, or an electrical sensor
 * with power or energy measurement. The polling task hands each value over;
 * it reaches the cluster on the CHIP task. */

#include "matter_mbm.h"
#include "matter.h"
#include "mbm_value.h"
extern "C" {
#include "app_config.h"
#include "aux_io.h"
}

#include <esp_log.h>
#include <esp_matter.h>
#include <freertos/FreeRTOS.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/clusters/electrical-power-measurement-server/ElectricalPowerMeasurementDelegate.h>
#include <app/clusters/power-topology-server/PowerTopologyDelegate.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <app/reporting/reporting.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <data_model_provider/clusters/electrical_energy_measurement/integration.h>
#include <platform/PlatformManager.h>

/* Root, the outputs, the inputs (isolated ones and header pads) and one
   endpoint per Modbus row: all of them at once must fit. */
static_assert(CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT >=
              1 + APP_CFG_DO_COUNT + APP_CFG_DI_ISOLATED_COUNT + AUX_PAD_COUNT + APP_CFG_MBM_COUNT,
              "CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT too small for every endpoint");

#define TAG "matter_mbm"

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

namespace {

constexpr int64_t kInt62 = INT64_C(4611686018427387904);   /* 2^62 */

/* ElectricalPowerMeasurement reads its attributes from a delegate. Only
   ActivePower is measured here; everything else is null. */
class PowerDelegate : public ElectricalPowerMeasurement::Delegate {
public:
    void Set(int64_t mw) { mActivePower.SetNonNull(mw); }

    ElectricalPowerMeasurement::PowerModeEnum GetPowerMode() override
    {
        return ElectricalPowerMeasurement::PowerModeEnum::kAc;
    }
    uint8_t GetNumberOfMeasurementTypes() override { return 1; }

    CHIP_ERROR StartAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetAccuracyByIndex(uint8_t i, ElectricalPowerMeasurement::Structs::MeasurementAccuracyStruct::Type &a) override
    {
        if (i > 0) return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        /* The spec wants an accuracy per measured quantity. The board does
           not know the meter's; it passes on what the meter says. */
        static const ElectricalPowerMeasurement::Structs::MeasurementAccuracyRangeStruct::Type kRange = {
            .rangeMin = -kInt62,
            .rangeMax = kInt62,
            .fixedMax = chip::MakeOptional(static_cast<uint64_t>(0)),
        };
        a.measurementType  = ElectricalPowerMeasurement::MeasurementTypeEnum::kActivePower;
        a.measured         = true;
        a.minMeasuredValue = -kInt62;
        a.maxMeasuredValue = kInt62;
        a.accuracyRanges   = chip::app::DataModel::List<const ElectricalPowerMeasurement::Structs::MeasurementAccuracyRangeStruct::Type>(&kRange, 1);
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR EndAccuracyRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetRangeByIndex(uint8_t, ElectricalPowerMeasurement::Structs::MeasurementRangeStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndRangesRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicCurrentsByIndex(uint8_t, ElectricalPowerMeasurement::Structs::HarmonicMeasurementStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicPhasesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicPhasesByIndex(uint8_t, ElectricalPowerMeasurement::Structs::HarmonicMeasurementStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndHarmonicPhasesRead() override { return CHIP_NO_ERROR; }

    chip::app::DataModel::Nullable<int64_t> GetActivePower() override { return mActivePower; }
    chip::app::DataModel::Nullable<int64_t> GetVoltage() override         { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetActiveCurrent() override   { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetReactiveCurrent() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetApparentCurrent() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetReactivePower() override   { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetApparentPower() override   { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetRMSVoltage() override      { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetRMSCurrent() override      { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetRMSPower() override        { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetFrequency() override       { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetPowerFactor() override     { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetNeutralCurrent() override  { return {}; }

private:
    chip::app::DataModel::Nullable<int64_t> mActivePower;
};

/* PowerTopology (mandatory on an electrical sensor) as node topology: the
   measurement is of the whole node, there are no other endpoints to list. */
class NodeTopology : public PowerTopology::Delegate {
public:
    CHIP_ERROR GetAvailableEndpointAtIndex(size_t, chip::EndpointId &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR GetActiveEndpointAtIndex(size_t, chip::EndpointId &) override    { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
};

NodeTopology  s_topology;
PowerDelegate s_power[APP_CFG_MBM_COUNT];

/* Per row: its endpoint (0 = none) and the kind it was made for. The
   endpoint set is fixed once the stack runs; a row changed afterwards keeps
   its old endpoint until the next boot, so the kind is checked before use. */
uint16_t s_ep[APP_CFG_MBM_COUNT];
uint8_t  s_kind[APP_CFG_MBM_COUNT];

/* Values from the polling task, taken over on the CHIP task. */
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
int64_t      s_val[APP_CFG_MBM_COUNT];
bool         s_fresh[APP_CFG_MBM_COUNT];
bool         s_scheduled;

/* Only a row that is read here: enabled, named, and on RS-485 -- this
   firmware has no Modbus TCP master. */
bool row_is_sensor(const app_config_t *cfg, int i)
{
    const mbm_poll_t *e = &cfg->mbm[i];
    return cfg->modbus.enable && cfg->modbus.rs485_role == MB_ROLE_MASTER &&
           e->enable && e->name[0] && e->transport == MBM_TRANSPORT_RTU &&
           e->matter != MBM_MATTER_NONE && e->matter <= MBM_MATTER_MAX;
}

esp_matter_attr_val_t nullable_u16(int64_t v) { return esp_matter_nullable_uint16((uint16_t)v); }
esp_matter_attr_val_t nullable_i16(int64_t v) { return esp_matter_nullable_int16((int16_t)v); }

/* On the CHIP task, stack locked. */
void report(int i, int64_t v)
{
    uint16_t ep = s_ep[i];
    switch (s_kind[i]) {
    case MBM_MATTER_TEMPERATURE: {
        /* A code-driven cluster: attribute::update() does not reach it. */
        chip::app::ConcreteClusterPath path(ep, TemperatureMeasurement::Id);
        auto *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
        if (iface)
            (void)static_cast<TemperatureMeasurementCluster *>(iface)->SetMeasuredValue(
                chip::app::DataModel::MakeNullable(static_cast<int16_t>(v)));
        break;
    }
    case MBM_MATTER_HUMIDITY: {
        esp_matter_attr_val_t a = nullable_u16(v);
        attribute::update(ep, RelativeHumidityMeasurement::Id,
                          RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &a);
        break;
    }
    case MBM_MATTER_PRESSURE: {
        esp_matter_attr_val_t a = nullable_i16(v);
        attribute::update(ep, PressureMeasurement::Id, PressureMeasurement::Attributes::MeasuredValue::Id, &a);
        break;
    }
    case MBM_MATTER_FLOW: {
        esp_matter_attr_val_t a = nullable_u16(v);
        attribute::update(ep, FlowMeasurement::Id, FlowMeasurement::Attributes::MeasuredValue::Id, &a);
        break;
    }
    case MBM_MATTER_POWER:
        s_power[i].Set(v);
        MatterReportingAttributeChangeCallback(ep, ElectricalPowerMeasurement::Id,
                                               ElectricalPowerMeasurement::Attributes::ActivePower::Id);
        break;
    case MBM_MATTER_ENERGY: {
        ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type m;
        m.energy = v;
        (void)ElectricalEnergyMeasurement::NotifyCumulativeEnergyMeasured(
            ep, chip::MakeOptional(m), chip::NullOptional);
        break;
    }
    default:
        break;
    }
}

/* On the CHIP task: everything that has arrived since the last run. A value
   arriving while this runs either is taken in the loop or finds
   s_scheduled cleared and schedules the next run itself. */
void flush(intptr_t)
{
    portENTER_CRITICAL(&s_mux);
    s_scheduled = false;
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++) {
        portENTER_CRITICAL(&s_mux);
        bool    fresh = s_fresh[i];
        int64_t v     = s_val[i];
        s_fresh[i]    = false;
        portEXIT_CRITICAL(&s_mux);
        if (fresh) report(i, v);
    }
}

} // namespace

esp_err_t matter_mbm_create(esp_matter::node_t *node)
{
    const app_config_t *cfg = app_config_get();
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++) {
        if (!row_is_sensor(cfg, i)) continue;
        uint8_t k = cfg->mbm[i].matter;
        endpoint_t *ep = nullptr;
        switch (k) {
        case MBM_MATTER_TEMPERATURE: { temperature_sensor::config_t c; ep = temperature_sensor::create(node, &c, ENDPOINT_FLAG_NONE, nullptr); break; }
        case MBM_MATTER_HUMIDITY:    { humidity_sensor::config_t c;    ep = humidity_sensor::create(node, &c, ENDPOINT_FLAG_NONE, nullptr);    break; }
        case MBM_MATTER_PRESSURE:    { pressure_sensor::config_t c;    ep = pressure_sensor::create(node, &c, ENDPOINT_FLAG_NONE, nullptr);    break; }
        case MBM_MATTER_FLOW:        { flow_sensor::config_t c;        ep = flow_sensor::create(node, &c, ENDPOINT_FLAG_NONE, nullptr);        break; }
        case MBM_MATTER_POWER:
        case MBM_MATTER_ENERGY: {
            electrical_sensor::config_t c;
            c.power_topology.feature_flags = cluster::power_topology::feature::node_topology::get_id();
            c.power_topology.delegate      = &s_topology;
            if (k == MBM_MATTER_POWER) {
                auto &p = c.with_electrical_power_measurement();
                p.feature_flags = cluster::electrical_power_measurement::feature::alternating_current::get_id();
                p.delegate      = &s_power[i];
            } else {
                auto &e = c.with_electrical_energy_measurement();
                e.feature_flags = cluster::electrical_energy_measurement::feature::imported_energy::get_id() |
                                  cluster::electrical_energy_measurement::feature::cumulative_energy::get_id();
            }
            ep = electrical_sensor::create(node, &c, ENDPOINT_FLAG_NONE, nullptr);
            break;
        }
        default:
            continue;
        }
        /* The rest of the device stays on Matter without this value. */
        if (!ep) {
            ESP_LOGE(TAG, "Modbus value %d (%s): endpoint not created", i + 1, cfg->mbm[i].name);
            continue;
        }
        s_ep[i]   = endpoint::get_id(ep);
        s_kind[i] = k;
        ESP_LOGI(TAG, "Modbus value %d (%s) endpoint_id=%u", i + 1, cfg->mbm[i].name, s_ep[i]);
    }
    return ESP_OK;
}

bool matter_mbm_endpoints_differ(const app_config_t *a, const app_config_t *b)
{
    for (int i = 0; i < APP_CFG_MBM_COUNT; i++) {
        uint8_t ka = row_is_sensor(a, i) ? a->mbm[i].matter : MBM_MATTER_NONE;
        uint8_t kb = row_is_sensor(b, i) ? b->mbm[i].matter : MBM_MATTER_NONE;
        if (ka != kb) return true;
    }
    return false;
}

void matter_mbm_update(uint8_t index, uint8_t kind, double value)
{
    /* A row given another kind since boot keeps its old endpoint, unanswered. */
    if (index >= APP_CFG_MBM_COUNT || !s_ep[index] || kind != s_kind[index]) return;

    int64_t v;
    if (!mbm_matter_value(s_kind[index], value, &v)) {
        ESP_LOGD(TAG, "Modbus value %u: %.10g outside the Matter attribute's range", index + 1, value);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_val[index]   = v;
    s_fresh[index] = true;
    bool schedule  = !s_scheduled;
    s_scheduled    = true;
    portEXIT_CRITICAL(&s_mux);
    /* One pending work item at a time, however many values arrive before
       the CHIP task gets to it. */
    if (schedule && chip::DeviceLayer::PlatformMgr().ScheduleWork(flush) != CHIP_NO_ERROR) {
        portENTER_CRITICAL(&s_mux);
        s_scheduled = false;
        portEXIT_CRITICAL(&s_mux);
    }
}

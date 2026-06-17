/* Matter integration for Waveshare ESP32-S3-POE-ETH-8DI-8DO
 *
 * Device model:
 *   Endpoints 1–8  : On/Off Light  (DO1–DO8, logical output state)
 *   Endpoints 9–16 : Contact Sensor (DI1–DI8, logical input state)
 *
 * Commissioning: Bluetooth LE PASE (standard Matter over Wi-Fi).
 * The existing Wi-Fi / Ethernet provisioning runs in parallel; Matter
 * commissioning is independent and uses the NVS "matter" partition.
 */

#include "matter.h"

extern "C" {
#include "dout.h"
#include "app_config.h"
}

#include <esp_log.h>
#include <esp_matter.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>

#define TAG "matter"
#define NUM_CHANNELS 8

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

/* Endpoint IDs assigned after node creation — index 0 = channel 1 */
static uint16_t s_do_ep[NUM_CHANNELS];
static uint16_t s_di_ep[NUM_CHANNELS];

/* Called by Matter stack when a controller writes an attribute (e.g. toggling a DO). */
static esp_err_t attr_update_cb(attribute::callback_type_t type,
                                uint16_t endpoint_id,
                                uint32_t cluster_id,
                                uint32_t attribute_id,
                                esp_matter_attr_val_t *val,
                                void *priv_data)
{
    if (type != PRE_UPDATE) return ESP_OK;

    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        /* Find which DO channel this endpoint corresponds to */
        for (int i = 0; i < NUM_CHANNELS; i++) {
            if (s_do_ep[i] == endpoint_id) {
                bool new_state = val->val.b;
                ESP_LOGI(TAG, "Matter → DO%d = %s", i + 1, new_state ? "ON" : "OFF");
                dout_set((uint8_t)i, new_state);
                return ESP_OK;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t identification_cb(identification::callback_type_t type,
                                   uint16_t endpoint_id,
                                   uint8_t effect_id,
                                   uint8_t effect_variant,
                                   void *priv_data)
{
    ESP_LOGI(TAG, "Identify callback: endpoint %u effect %u", endpoint_id, effect_id);
    return ESP_OK;
}

static void event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Matter commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kInternetConnectivityChange:
        ESP_LOGI(TAG, "Matter internet connectivity change");
        break;
    default:
        break;
    }
}

esp_err_t matter_init(void)
{
    const app_config_t *cfg = app_config_get();

    /* Create the root node */
    node::config_t node_cfg;
    node_t *node = node::create(&node_cfg, attr_update_cb, identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return ESP_FAIL;
    }

    /* Create 8 On/Off Light endpoints for digital outputs */
    for (int i = 0; i < NUM_CHANNELS; i++) {
        on_off_light::config_t cfg_do = {};
        cfg_do.on_off.on_off = dout_get((uint8_t)i);

        endpoint_t *ep = on_off_light::create(node, &cfg_do, ENDPOINT_FLAG_NONE, (void *)(intptr_t)i);
        if (!ep) {
            ESP_LOGE(TAG, "Failed to create DO%d endpoint", i + 1);
            return ESP_FAIL;
        }
        s_do_ep[i] = endpoint::get_id(ep);
        ESP_LOGI(TAG, "DO%d endpoint_id=%u (name: %s)",
                 i + 1, s_do_ep[i],
                 cfg->dout[i].name[0] ? cfg->dout[i].name : "-");
    }

    /* Create 8 Contact Sensor endpoints for digital inputs */
    for (int i = 0; i < NUM_CHANNELS; i++) {
        contact_sensor::config_t cfg_di = {};
        /* BooleanState StateValue: true = contact (closed), false = open */
        cfg_di.boolean_state.state_value = false;

        endpoint_t *ep = contact_sensor::create(node, &cfg_di, ENDPOINT_FLAG_NONE, (void *)(intptr_t)(NUM_CHANNELS + i));
        if (!ep) {
            ESP_LOGE(TAG, "Failed to create DI%d endpoint", i + 1);
            return ESP_FAIL;
        }
        s_di_ep[i] = endpoint::get_id(ep);
        ESP_LOGI(TAG, "DI%d endpoint_id=%u (name: %s)",
                 i + 1, s_di_ep[i],
                 cfg->di[i].name[0] ? cfg->di[i].name : "-");
    }

    /* Start the Matter stack */
    esp_err_t err = esp_matter::start(event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Matter stack started — %d DO + %d DI endpoints", NUM_CHANNELS, NUM_CHANNELS);
    return ESP_OK;
}

void matter_di_update(uint8_t channel, bool active)
{
    if (channel >= NUM_CHANNELS) return;
    uint16_t ep_id = s_di_ep[channel];
    if (ep_id == 0) return;  /* not initialised yet */

    /* BooleanState::StateValue = true means "contact detected" (closed).
       For a generic DI, active logical state maps to contact detected. */
    esp_matter_attr_val_t val = esp_matter_bool(active);
    attribute::update(ep_id,
                      BooleanState::Id,
                      BooleanState::Attributes::StateValue::Id,
                      &val);
}

void matter_do_update(uint8_t channel, bool state)
{
    if (channel >= NUM_CHANNELS) return;
    uint16_t ep_id = s_do_ep[channel];
    if (ep_id == 0) return;

    esp_matter_attr_val_t val = esp_matter_bool(state);
    attribute::update(ep_id,
                      OnOff::Id,
                      OnOff::Attributes::OnOff::Id,
                      &val);
}

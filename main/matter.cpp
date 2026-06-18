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

#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_random.h>
#include <nvs.h>
#include <inttypes.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/server/Server.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/Base64.h>
#include <setup_payload/OnboardingCodesUtil.h>

/* Declared in esp-matter/data_model_provider/clusters/boolean_state/integration.cpp */
esp_err_t esp_matter_boolean_state_set_value(chip::EndpointId endpoint_id, bool value);

/* Stored after matter_init() succeeds — read-only from web_server task */
static char s_qr_code[128];
static char s_manual_code[32];
static bool s_commissioned;

extern "C" {
const char *matter_get_qr_code(void)     { return s_qr_code; }
const char *matter_get_manual_code(void) { return s_manual_code; }
bool        matter_is_commissioned(void) { return s_commissioned; }
}

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

/* Matter spec §5.1.6.1 — passcodes that are trivially guessable are forbidden. */
static const uint32_t kForbiddenPasscodes[] = {
    0u, 11111111u, 22222222u, 33333333u, 44444444u,
    55555555u, 66666666u, 77777777u, 88888888u, 99999999u,
    12345678u, 87654321u,
};

static bool passcode_is_forbidden(uint32_t code)
{
    for (size_t i = 0; i < sizeof(kForbiddenPasscodes) / sizeof(kForbiddenPasscodes[0]); i++) {
        if (code == kForbiddenPasscodes[i]) return true;
    }
    return false;
}

/* Generate and persist unique commissioning credentials on first boot.
 *
 * Writes to the "chip-factory" namespace in the default NVS partition.
 * LegacyTemporaryCommissionableDataProvider reads these keys at stack startup
 * (falls back to the Kconfig test values when any key is absent).
 *
 * Must be called BEFORE esp_matter::start() so the provider sees the new
 * values when it initialises.
 */
static esp_err_t matter_provision_if_needed(void)
{
    /* Already provisioned? */
    {
        nvs_handle_t h;
        bool done = false;
        if (nvs_open_from_partition("nvs", "chip-factory", NVS_READONLY, &h) == ESP_OK) {
            uint32_t v;
            done = (nvs_get_u32(h, "pin-code", &v) == ESP_OK);
            nvs_close(h);
        }
        if (done) {
            ESP_LOGI(TAG, "Matter credentials already provisioned");
            return ESP_OK;
        }
    }

    /* Random passcode: 1–99999998, not a spec-forbidden value */
    uint32_t passcode;
    do {
        passcode = (esp_random() % 99999998u) + 1u;
    } while (passcode_is_forbidden(passcode));

    /* Random 12-bit discriminator */
    uint32_t discriminator = esp_random() & 0xFFFu;

    /* Random 16-byte PBKDF2 salt */
    uint8_t salt[chip::Crypto::kSpake2p_Min_PBKDF_Salt_Length];
    esp_fill_random(salt, sizeof(salt));

    /* Compute SPAKE2+ verifier.
     * Iteration count must match what the stack reads on the next boot.
     * LegacyTemporaryCommissionableDataProvider falls back to
     * CHIP_DEVICE_CONFIG_USE_TEST_SPAKE2P_ITERATION_COUNT = 1000. */
    static constexpr uint32_t kIter = 1000;
    chip::Crypto::Spake2pVerifier verifier;
    if (verifier.Generate(kIter, chip::ByteSpan(salt, sizeof(salt)), passcode) != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "SPAKE2+ verifier generation failed");
        return ESP_FAIL;
    }

    chip::Crypto::Spake2pVerifierSerialized verifier_bytes;
    chip::MutableByteSpan verifier_span(verifier_bytes);
    if (verifier.Serialize(verifier_span) != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "SPAKE2+ verifier serialization failed");
        return ESP_FAIL;
    }

    /* Base64-encode salt and verifier — format expected by ReadConfigValueStr */
    char salt_b64[BASE64_ENCODED_LEN(sizeof(salt)) + 1];
    uint32_t salt_b64_len = chip::Base64Encode32(salt, sizeof(salt), salt_b64);
    salt_b64[salt_b64_len] = '\0';

    char ver_b64[BASE64_ENCODED_LEN(chip::Crypto::kSpake2p_VerifierSerialized_Length) + 1];
    uint32_t ver_b64_len = chip::Base64Encode32(
        verifier_bytes, chip::Crypto::kSpake2p_VerifierSerialized_Length, ver_b64);
    ver_b64[ver_b64_len] = '\0';

    /* Persist */
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition("nvs", "chip-factory", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open chip-factory NVS: %s", esp_err_to_name(err));
        return err;
    }

    if ((err = nvs_set_u32(h, "pin-code",     passcode))      != ESP_OK ||
        (err = nvs_set_u32(h, "discriminator", discriminator)) != ESP_OK ||
        (err = nvs_set_str(h, "salt",          salt_b64))      != ESP_OK ||
        (err = nvs_set_str(h, "verifier",      ver_b64))       != ESP_OK ||
        (err = nvs_commit(h))                                   != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write Matter credentials: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Matter: generated passcode=%" PRIu32 " discriminator=0x%03" PRIx32,
                 passcode, discriminator);
    }

    nvs_close(h);
    return err;
}

esp_err_t matter_init(void)
{
    /* Provision unique credentials on first boot before starting the stack */
    if (matter_provision_if_needed() != ESP_OK) {
        ESP_LOGW(TAG, "Provisioning failed — falling back to default test credentials");
    }

    const app_config_t *cfg = app_config_get();

    /* Create the root node */
    node::config_t node_cfg;
    node_t *node = node::create(&node_cfg, attr_update_cb, identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return ESP_FAIL;
    }

    /* Create 8 On/Off Plug-In Unit endpoints for digital outputs */
    for (int i = 0; i < NUM_CHANNELS; i++) {
        on_off_plug_in_unit::config_t cfg_do = {};
        cfg_do.on_off.on_off = dout_get((uint8_t)i);

        endpoint_t *ep = on_off_plug_in_unit::create(node, &cfg_do, ENDPOINT_FLAG_NONE, (void *)(intptr_t)i);
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

    s_commissioned = chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
    if (s_commissioned) {
        ESP_LOGI(TAG, "Device already commissioned — showing codes for re-commissioning after factory reset");
    } else {
        ESP_LOGI(TAG, "Device not yet commissioned — scan QR code or enter manual code to pair");
    }

    chip::RendezvousInformationFlags rvFlags(chip::RendezvousInformationFlag::kBLE);

    chip::MutableCharSpan qr_span(s_qr_code, sizeof(s_qr_code) - 1);
    if (GetQRCode(qr_span, rvFlags) == CHIP_NO_ERROR) {
        s_qr_code[qr_span.size()] = '\0';
    }

    chip::MutableCharSpan man_span(s_manual_code, sizeof(s_manual_code) - 1);
    if (GetManualPairingCode(man_span, rvFlags) == CHIP_NO_ERROR) {
        s_manual_code[man_span.size()] = '\0';
    }

    PrintOnboardingCodes(rvFlags);

    return ESP_OK;
}

void matter_di_update(uint8_t channel, bool active)
{
    if (channel >= NUM_CHANNELS) return;
    uint16_t ep_id = s_di_ep[channel];
    if (ep_id == 0) return;  /* not initialised yet */

    /* StateValue is owned by BooleanStateCluster (ATTRIBUTE_FLAG_MANAGED_INTERNALLY).
       Must call SetStateValue() through the registered cluster, not attribute::update(). */
    esp_err_t err = esp_matter_boolean_state_set_value(ep_id, active);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGE(TAG, "DI%d state update failed: %s", channel + 1, esp_err_to_name(err));
    }
}

void matter_decommission(void)
{
    /* ScheduleWork posts to the CHIP task queue — safe to call from any thread */
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        ESP_LOGI(TAG, "Decommission: removing all Matter fabrics");

        /* Remove fabrics and send Leave events to any active controllers */
        chip::Server::GetInstance().GetFabricTable().DeleteAllFabrics();
        chip::DeviceLayer::PlatformMgr().HandleServerShuttingDown();

        /* Erase Matter operational NVS namespaces.
         * chip-factory (our passcode/discriminator) is intentionally left intact.
         * WiFi credentials (managed by esp32-wifi-bootstrap) are also preserved. */
        static const char * const kMatterNS[] = { "chip-config", "chip-counters", "CHIP_KVS" };
        for (size_t i = 0; i < sizeof(kMatterNS) / sizeof(kMatterNS[0]); i++) {
            nvs_handle_t h;
            if (nvs_open_from_partition("nvs", kMatterNS[i], NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_all(h);
                nvs_commit(h);
                nvs_close(h);
            }
        }

        ESP_LOGI(TAG, "Decommission complete — rebooting");
        esp_restart();
    });

    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "matter_decommission: ScheduleWork failed");
    }
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

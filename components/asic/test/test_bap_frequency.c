#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "main/global_state.h"
#include "main/bap/bap.h"
#define TEST_STUB_GLOBAL_STATE_H

#define nvs_config_get_string frequency_test_get_string
#define nvs_config_set_string frequency_test_set_string
#define nvs_config_set_float frequency_test_set_float
#define nvs_config_set_u16 frequency_test_set_u16
#define nvs_config_set_bool frequency_test_set_bool
#define esp_restart frequency_test_restart
#define esp_timer_get_time frequency_test_time
#define get_wifi_current_rssi frequency_test_rssi
#define BAP_send_message frequency_test_send
#define BAP_send_message_with_queue frequency_test_send
#include "nvs_config.h"
int64_t frequency_test_time(void);
void frequency_test_restart(void);
esp_err_t frequency_test_rssi(int8_t *rssi);
void frequency_test_send(bap_command_t command, const char *parameter, const char *value);
#define TAG frequency_protocol_tag
#include "main/bap/bap_protocol.c"
#undef TAG
#define TAG frequency_handlers_tag
#include "main/bap/bap_handlers.c"
#undef TAG
#define TAG frequency_subscription_tag
#include "main/bap/bap_subscription.c"
#undef TAG

QueueHandle_t bap_uart_send_queue;
SemaphoreHandle_t bap_uart_send_mutex;
SemaphoreHandle_t bap_subscription_mutex;
GlobalState *bap_global_state;

static int64_t clock_us;
static unsigned sent;
static struct {
    bap_command_t command;
    char parameter[32];
    char value[64];
} responses[16];

int64_t frequency_test_time(void) { return clock_us; }
char *frequency_test_get_string(NvsConfigKey key) { (void)key; return strdup(""); }
void frequency_test_set_string(NvsConfigKey key, const char *value) { (void)key; (void)value; }
void frequency_test_set_float(NvsConfigKey key, float value) { (void)key; (void)value; }
void frequency_test_set_u16(NvsConfigKey key, uint16_t value) { (void)key; (void)value; }
void frequency_test_set_bool(NvsConfigKey key, bool value) { (void)key; (void)value; }
void frequency_test_restart(void) { TEST_FAIL_MESSAGE("Unexpected restart"); abort(); }
esp_err_t frequency_test_rssi(int8_t *rssi) { *rssi = -42; return ESP_OK; }
void frequency_test_send(bap_command_t command, const char *parameter, const char *value)
{
    TEST_ASSERT_LESS_THAN(16, sent);
    responses[sent].command = command;
    snprintf(responses[sent].parameter, sizeof(responses[sent].parameter), "%s", parameter);
    snprintf(responses[sent].value, sizeof(responses[sent].value), "%s", value ? value : "");
    ++sent;
}

static GlobalState *fixture(void)
{
    GlobalState *state = calloc(1, sizeof(*state));
    TEST_ASSERT_NOT_NULL(state);
    state->DEVICE_CONFIG.family.name = "Bonanza";
    state->DEVICE_CONFIG.family.asic.name = "BZM";
    state->SYSTEM_MODULE.is_connected = true;
    state->SYSTEM_MODULE.current_hashrate = 1200.55f;
    state->SYSTEM_MODULE.pools[0].url = "pool.example";
    state->SYSTEM_MODULE.pools[0].user = "miner";
    state->SYSTEM_MODULE.pools[0].port = 3333;
    state->POWER_MANAGEMENT_MODULE.actual_frequency = 800;
    state->POWER_MANAGEMENT_MODULE.frequency_value = 1350;
    bap_global_state = state;
    bap_subscription_mutex = xSemaphoreCreateMutex();
    TEST_ASSERT_NOT_NULL(bap_subscription_mutex);
    clock_us += 10000000;
    sent = 0;
    TEST_ASSERT_EQUAL(ESP_OK, BAP_handlers_init(state));
    TEST_ASSERT_EQUAL(ESP_OK, BAP_subscription_init());
    return state;
}

static void release_fixture(GlobalState *state)
{
    free(state);
    bap_global_state = NULL;
    vSemaphoreDelete(bap_subscription_mutex);
    bap_subscription_mutex = NULL;
}

static void command(const char *body, int checksum)
{
    char message[BAP_MAX_MESSAGE_LEN];
    if (checksum) {
        uint8_t value = BAP_calculate_checksum(body) ^ (checksum < 0 ? 1 : 0);
        snprintf(message, sizeof(message), "$%s*%02X\r\n", body, value);
    } else {
        snprintf(message, sizeof(message), "$%s\r\n", body);
    }
    BAP_parse_message(message);
}

static void expect(unsigned index, bap_command_t cmd, const char *parameter, const char *value)
{
    TEST_ASSERT_TRUE(index < sent);
    TEST_ASSERT_EQUAL(cmd, responses[index].command);
    TEST_ASSERT_EQUAL_STRING(parameter, responses[index].parameter);
    TEST_ASSERT_EQUAL_STRING(value, responses[index].value);
}

TEST_CASE("BAP frequency subscription pushes actual MHz changes and stops on unsubscribe", "[asic][bap][frequency]")
{
    GlobalState *state = fixture();
    command("BAP,SUB,frequency,1000", 1);
    expect(0, BAP_CMD_ACK, "frequency", "subscribed");
    BAP_send_subscription_update(state);
    expect(1, BAP_CMD_RES, "frequency", "800.00");
    state->POWER_MANAGEMENT_MODULE.actual_frequency = 1000;
    clock_us += 999000;
    BAP_send_subscription_update(state);
    TEST_ASSERT_EQUAL(2, sent);
    clock_us += 1000;
    BAP_send_subscription_update(state);
    expect(2, BAP_CMD_RES, "frequency", "1000.00");
    clock_us += 1000000;
    BAP_send_subscription_update(state);
    TEST_ASSERT_EQUAL(3, sent);
    state->POWER_MANAGEMENT_MODULE.actual_frequency = 0;
    clock_us += 1000000;
    BAP_send_subscription_update(state);
    expect(3, BAP_CMD_RES, "frequency", "0.00");
    command("BAP,UNSUB,frequency", 1);
    expect(4, BAP_CMD_ACK, "frequency", "unsubscribed");
    state->POWER_MANAGEMENT_MODULE.actual_frequency = 1200;
    clock_us += 1000000;
    BAP_send_subscription_update(state);
    TEST_ASSERT_EQUAL(5, sent);
    release_fixture(state);
}

TEST_CASE("BAP frequency subscriptions renew snapshots and retain the five-minute expiry", "[asic][bap][frequency]")
{
    GlobalState *state = fixture();
    command("BAP,SUB,frequency,1000", 1);
    BAP_send_subscription_update(state);
    expect(1, BAP_CMD_RES, "frequency", "800.00");
    clock_us += 60000000;
    command("BAP,SUB,frequency,1000", 1);
    expect(2, BAP_CMD_ACK, "frequency", "subscribed");
    BAP_send_subscription_update(state);
    expect(3, BAP_CMD_RES, "frequency", "800.00");
    clock_us += 300001000;
    BAP_send_subscription_update(state);
    expect(4, BAP_CMD_STA, "frequency", "subscription_timeout");
    TEST_ASSERT_EQUAL(5, sent);
    command("BAP,SUB,frequency,1000", 1);
    BAP_send_subscription_update(state);
    expect(6, BAP_CMD_RES, "frequency", "800.00");
    release_fixture(state);
}

TEST_CASE("BAP frequency retains AP gating and legacy subscription input handling", "[asic][bap][frequency]")
{
    GlobalState *state = fixture();
    state->SYSTEM_MODULE.is_connected = false;
    command("BAP,SUB,frequency,1000", 1);
    expect(0, BAP_CMD_ERR, "frequency", "ap_mode_no_subscriptions");
    BAP_send_subscription_update(state);
    TEST_ASSERT_EQUAL(1, sent);
    release_fixture(state);
    const struct {
        const char *body;
        int checksum;
        uint32_t interval_ms;
    } cases[] = {
        {"BAP,SUB,frequency", 1, 3000},
        {"BAP,SUB,frequency,50", 0, 50},
        {"BAP,SUB,frequency,1000oops", -1, 1000},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        state = fixture();
        command(cases[i].body, cases[i].checksum);
        expect(0, BAP_CMD_ACK, "frequency", "subscribed");
        BAP_send_subscription_update(state);
        expect(1, BAP_CMD_RES, "frequency", "800.00");
        state->POWER_MANAGEMENT_MODULE.actual_frequency = 1000;
        clock_us += (cases[i].interval_ms - 1) * 1000;
        BAP_send_subscription_update(state);
        TEST_ASSERT_EQUAL(2, sent);
        clock_us += 1000;
        BAP_send_subscription_update(state);
        expect(2, BAP_CMD_RES, "frequency", "1000.00");
        release_fixture(state);
    }
}

TEST_CASE("BAP frequency is opt-in and preserves identity and existing subscriptions", "[asic][bap][frequency]")
{
    GlobalState *state = fixture();
    command("BAP,REQ,systemInfo", 1);
    expect(0, BAP_CMD_RES, "deviceModel", "Bonanza");
    expect(1, BAP_CMD_RES, "asicModel", "BZM");
    expect(2, BAP_CMD_RES, "pool", "pool.example");
    expect(3, BAP_CMD_RES, "poolPort", "3333");
    expect(4, BAP_CMD_RES, "poolUser", "miner");
    TEST_ASSERT_EQUAL(5, sent);
    sent = 0;
    command("BAP,SUB,hashrate,1000", 1);
    command("BAP,SUB,frequency,1000", 1);
    BAP_send_subscription_update(state);
    expect(2, BAP_CMD_RES, "hashrate", "1200.55");
    expect(3, BAP_CMD_RES, "frequency", "800.00");
    state->POWER_MANAGEMENT_MODULE.actual_frequency = 1000;
    clock_us += 1000000;
    BAP_send_subscription_update(state);
    expect(4, BAP_CMD_RES, "frequency", "1000.00");
    clock_us += 1000000;
    BAP_send_subscription_update(state);
    TEST_ASSERT_EQUAL(5, sent);
    command("BAP,UNSUB,frequency", 1);
    expect(5, BAP_CMD_ACK, "frequency", "unsubscribed");
    state->SYSTEM_MODULE.current_hashrate = 2222;
    state->POWER_MANAGEMENT_MODULE.actual_frequency = 1200;
    clock_us += 1000000;
    BAP_send_subscription_update(state);
    expect(6, BAP_CMD_RES, "hashrate", "2222.00");
    TEST_ASSERT_EQUAL(7, sent);
    release_fixture(state);
}

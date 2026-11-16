/*
 * cloud.cpp - see cloud.h. Built only with -DCOLDTRACE_CLOUD.
 *
 * Threads: esp-mqtt runs its own FreeRTOS task and calls on_mqtt() from it.
 * That callback never touches the logger; it only sets a flag or posts to a
 * queue. cloud_loop(), on the Arduino loop task, drains the queue and calls
 * into the core, so the core stays single-threaded.
 */
#ifdef COLDTRACE_CLOUD

#include "cloud.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <time.h>

#include "azure_ca.h"
#include "mqtt_client.h"

#if __has_include("secrets.h")
#include "secrets.h"            /* HUB_HOST, DEVICE_ID, SAS_TOKEN; git-ignored */
#else
#pragma message "secrets.h not found: building with placeholder credentials (CI build)"
#include "secrets.example.h"
#endif

static const char    *WIFI_SSID      = "Wokwi-GUEST";   /* Wokwi's open virtual access point */
static const int      WIFI_CHANNEL   = 6;               /* skips the scan in Wokwi */
static const uint32_t MQTT_TIMEOUT_MS = 30000;          /* TLS handshake takes ~31 s simulated in Wokwi */

/* What the MQTT task tells the loop. */
typedef struct {
    enum { MSG_ACK, MSG_ERROR } kind;
    int a, b, c;                /* ACK: msg_id. ERROR: error type, connack code, TLS error */
} cloud_msg_t;

RTC_NOINIT_ATTR volatile uint32_t g_cloud_stage;

static esp_mqtt_client_handle_t s_mqtt;
static QueueHandle_t            s_queue;
static volatile bool            s_mqtt_up;
static bool                     s_mqtt_started;
static bool                     s_boot_id_set;
static bool                     s_fault_was_on;
static char                     s_topic[128];
static char                     s_user[160];
static char                     s_uri[128];

/* ---- net_t for the core --------------------------------------------------- */

static bool link_ready(void)
{
    return s_mqtt_up && WiFi.status() == WL_CONNECTED;
}

/* The core sees the link only once the boot id is fixed, so no reading can
 * go out under a boot id that is about to change. */
static bool net_connected(void *ctx)
{
    (void)ctx;
    return s_boot_id_set && link_ready();
}

static int net_publish(void *ctx, const char *payload, size_t len)
{
    (void)ctx;
    if (s_mqtt == nullptr) {
        return -1;
    }
    /* QoS 1: the broker answers with PUBACK, which esp-mqtt reports as
     * MQTT_EVENT_PUBLISHED carrying this message id. */
    uint32_t outer = g_cloud_stage;
    CLOUD_STAGE(STAGE_PUBLISH);
    int id = esp_mqtt_client_publish(s_mqtt, s_topic, payload, (int)len, 1, 0);
    CLOUD_STAGE(outer);
    return id;
}

static const net_t s_net = { net_connected, net_publish, nullptr };

/* ---- MQTT task callback: flags and queue only ------------------------------ */

static void on_mqtt(void *, esp_event_base_t, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    cloud_msg_t m = {};
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_up = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_up = false;
        break;
    case MQTT_EVENT_PUBLISHED:
        m.kind = cloud_msg_t::MSG_ACK;
        m.a = e->msg_id;
        xQueueSend(s_queue, &m, 0);          /* full queue: the ack is lost, the core resends */
        break;
    case MQTT_EVENT_ERROR:
        m.kind = cloud_msg_t::MSG_ERROR;
        m.a = e->error_handle->error_type;
        m.b = e->error_handle->connect_return_code;
        m.c = e->error_handle->esp_tls_stack_err;
        xQueueSend(s_queue, &m, 0);
        break;
    default:
        break;
    }
}

/* ---- setup ---------------------------------------------------------------------- */

static void mqtt_start(void)
{
    snprintf(s_uri, sizeof s_uri, "mqtts://%s:8883", HUB_HOST);
    snprintf(s_user, sizeof s_user, "%s/%s/?api-version=2021-04-12", HUB_HOST, DEVICE_ID);

    esp_mqtt_client_config_t cfg = {};
    cfg.uri = s_uri;
    cfg.client_id = DEVICE_ID;
    cfg.username = s_user;
    cfg.password = SAS_TOKEN;
    cfg.cert_pem = AZURE_ROOT_CA;
    cfg.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    cfg.keepalive = 60;
    cfg.network_timeout_ms = MQTT_TIMEOUT_MS;
    cfg.reconnect_timeout_ms = 5000;          /* esp-mqtt reconnects on its own */
    /* Below the loop task (2, set in setup()): the TLS handshake (~31 s
     * simulated) runs while loop() sleeps and can never starve it. The
     * default (5) starved loop() and tripped the 3 s task watchdog. */
    cfg.task_prio = 1;
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY, on_mqtt, nullptr);
    esp_mqtt_client_start(s_mqtt);
    s_mqtt_started = true;
}

static void wifi_join(void)
{
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, "", WIFI_CHANNEL);  /* returns at once; joins in the background */
}

void cloud_begin(logger_t *lg)
{
    if (lg->reset_reason == RESET_WATCHDOG) {
        Serial.printf("EVT WDT_STAGE %lu\r\n", (unsigned long)g_cloud_stage);
    }
    g_cloud_stage = 0;

    /* ESP-IDF logs share UART0 with the protocol; keep them to real errors
     * and report MQTT trouble as EVT NET_ERR lines instead. */
    esp_log_level_set("*", ESP_LOG_ERROR);

    s_queue = xQueueCreate(16, sizeof(cloud_msg_t));
    snprintf(s_topic, sizeof s_topic,
             "devices/%s/messages/events/$.ct=application%%2Fjson&$.ce=utf-8", DEVICE_ID);

    WiFi.mode(WIFI_STA);
    wifi_join();
    configTime(0, 0, "pool.ntp.org", "time.google.com");   /* for the boot id; never for latency */

    logger_attach_net(lg, &s_net, DEVICE_ID, esp_random());
}

/* ---- loop ------------------------------------------------------------------------ */

/* FAULT NET: make the outage real (Wi-Fi off), not just a flag in the core. */
static void apply_fault_net(logger_t *lg)
{
    bool on = lg->pub.fault_net;
    if (on == s_fault_was_on) {
        return;
    }
    s_fault_was_on = on;
    if (on) {
        CLOUD_STAGE(STAGE_FAULT_WIFI_OFF);
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);        /* radio stays on; MQTT drops with it */
    } else {
        CLOUD_STAGE(STAGE_FAULT_WIFI_ON);
        wifi_join();
    }
}

/* Fix the boot id the first time the link comes up, before anything is
 * published. Mixing in the clock keeps it unique across Wokwi runs even if
 * the simulator's random generator repeats. */
static void settle_boot_id(logger_t *lg)
{
    if (s_boot_id_set || !link_ready()) {
        return;
    }
    time_t now = time(nullptr);
    if (now > 1700000000) {                   /* NTP has set the clock */
        lg->pub.boot_id ^= (uint32_t)now;
    }
    s_boot_id_set = true;
}

void cloud_loop(logger_t *lg)
{
    apply_fault_net(lg);

    if (!s_mqtt_started && WiFi.status() == WL_CONNECTED) {
        CLOUD_STAGE(STAGE_MQTT_START);
        mqtt_start();
    }

    CLOUD_STAGE(STAGE_QUEUE);
    cloud_msg_t m;
    while (xQueueReceive(s_queue, &m, 0) == pdTRUE) {
        if (m.kind == cloud_msg_t::MSG_ACK) {
            logger_on_ack(lg, m.a);
        } else {
            Serial.printf("EVT NET_ERR type=%d connack=%d tls=0x%x\r\n", m.a, m.b, (unsigned)m.c);
        }
    }

    CLOUD_STAGE(STAGE_BOOT_ID);
    settle_boot_id(lg);
}

#endif /* COLDTRACE_CLOUD */

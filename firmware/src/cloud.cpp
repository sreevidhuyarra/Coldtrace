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
#include <esp_task_wdt.h>
#include <soc/timer_group_reg.h>
#include <soc/timer_group_struct.h>
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

static const size_t  DATA_MAX = 2048;                   /* largest twin/method message we accept */

/* What the MQTT task tells the loop. */
typedef struct {
    enum { MSG_ACK, MSG_ERROR, MSG_DATA } kind;
    int a, b, c;                /* ACK: msg_id. ERROR: error type, connack code, TLS error.
                                   DATA: topic length, payload length */
    char *buf;                  /* DATA: heap copy "topic\0payload\0", freed by the loop */
} cloud_msg_t;

/* Twin and direct-method topics (IoT Hub's MQTT conventions). */
static const char *T_TWIN_RES      = "$iothub/twin/res/";
static const char *T_TWIN_DESIRED  = "$iothub/twin/PATCH/properties/desired/";
static const char *T_METHOD        = "$iothub/methods/POST/";

RTC_NOINIT_ATTR volatile uint32_t g_cloud_stage;
/* Set just before a requested restart; checked and cleared at the next boot. */
RTC_NOINIT_ATTR static volatile uint32_t g_restart_requested;
static const uint32_t RESTART_MAGIC = 0x52535431u;   /* "RST1" */

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
        /* subscriptions don't survive a reconnect (clean session): renew them,
         * then ask for the full twin so settings changed while offline apply */
        esp_mqtt_client_subscribe(e->client, "$iothub/twin/res/#", 0);
        esp_mqtt_client_subscribe(e->client, "$iothub/twin/PATCH/properties/desired/#", 0);
        esp_mqtt_client_subscribe(e->client, "$iothub/methods/POST/#", 0);
        esp_mqtt_client_publish(e->client, "$iothub/twin/GET/?$rid=get", "", 0, 0, 0);
        s_mqtt_up = true;
        break;
    case MQTT_EVENT_DATA:
        /* whole messages only (twin docs and method calls are small); copy,
         * because the event's buffers are reused once this returns */
        if (e->current_data_offset == 0 && e->data_len == e->total_data_len &&
            e->topic_len > 0 && (size_t)(e->topic_len + e->data_len) < DATA_MAX) {
            char *buf = (char *)malloc((size_t)e->topic_len + (size_t)e->data_len + 2u);
            if (buf != nullptr) {
                memcpy(buf, e->topic, (size_t)e->topic_len);
                buf[e->topic_len] = '\0';
                memcpy(buf + e->topic_len + 1, e->data, (size_t)e->data_len);
                buf[e->topic_len + 1 + e->data_len] = '\0';
                m.kind = cloud_msg_t::MSG_DATA;
                m.a = e->topic_len;
                m.b = e->data_len;
                m.buf = buf;
                if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
                    free(buf);
                }
            }
        }
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
    WiFi.persistent(false);                   /* no flash writes of the Wi-Fi config: a flash
                                                 write stalls both CPUs (bug 005) */
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, "", WIFI_CHANNEL);  /* returns at once; joins in the background */
}

reset_reason_t cloud_check_requested_restart(reset_reason_t reason, bool *chained)
{
    /* A watchdog reset inside a restart we asked for (the flag was set right
     * before esp_restart() and nothing has run since) is still a software
     * reset; say so, and report the chain so it stays visible. A real hang
     * (FAULT HANG) never sets the flag, so it still reads WATCHDOG. */
    bool requested = (g_restart_requested == RESTART_MAGIC);
    g_restart_requested = 0;
    *chained = requested && reason == RESET_WATCHDOG;
    return *chained ? RESET_SOFTWARE : reason;
}

void cloud_begin(logger_t *lg, bool reset_chained)
{
    if (reset_chained) {
        Serial.printf("EVT RESET_CHAIN sw>wdt\r\n");
    } else if (lg->reset_reason == RESET_WATCHDOG) {
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

/* ---- restart ---------------------------------------------------------------------- */

void cloud_prepare_restart(void)
{
    /* A shutdown with the network up takes a moment; the reboot that follows
     * makes the task watchdog's guarantee moot, so stop watching the loop. */
    esp_task_wdt_delete(NULL);

    CLOUD_STAGE(STAGE_RESTART_MQTT);
    if (s_mqtt != nullptr) {
        esp_mqtt_client_stop(s_mqtt);
    }
    CLOUD_STAGE(STAGE_RESTART_WIFI);
    WiFi.disconnect(true, false);             /* radio off */
    delay(50);

    /* In Wokwi the first boot after RESET was killed 0.6 s into the bootloader
     * by the timer-group-1 watchdog (TG1WDT_SYS_RESET): the timer groups keep
     * their "flash boot protection" bit across a CPU reset, the bit re-arms
     * the watchdog while booting from flash, and loading this 920 KB image
     * takes longer than it allows. Clear the bit (and the watchdog) on both
     * timer groups; esp_restart() follows within microseconds. */
    CLOUD_STAGE(STAGE_RESTART_CALL);
    timg_dev_t *groups[] = { &TIMERG0, &TIMERG1 };
    for (timg_dev_t *g : groups) {
        g->wdt_wprotect = TIMG_WDT_WKEY_VALUE;
        g->wdt_config0.flashboot_mod_en = 0;
        g->wdt_config0.en = 0;
        g->wdt_wprotect = 0;
    }
    g_restart_requested = RESTART_MAGIC;      /* survives the reset: see cloud_begin() */
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

/* Once, after the MQTT task exists: print the real priority and CPU of the
 * loop and MQTT tasks (EVT TASKS loop=<prio>/<core> mqtt=<prio>/<affinity>),
 * so the scheduling assumptions in bug 005 are checked, not assumed. */
static void report_tasks(void)
{
    static bool s_done;
    if (s_done || !s_mqtt_started) {
        return;
    }
    TaskHandle_t mqtt = xTaskGetHandle("mqtt_task");
    if (mqtt == nullptr) {
        return;
    }
    s_done = true;
    BaseType_t aff = xTaskGetAffinity(mqtt);
    Serial.printf("EVT TASKS loop=%u/%d mqtt=%u/%s\r\n", (unsigned)uxTaskPriorityGet(NULL), xPortGetCoreID(),
                  (unsigned)uxTaskPriorityGet(mqtt),
                  aff == tskNO_AFFINITY ? "any" : (aff == 0 ? "0" : "1"));
}

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* "...?$rid=abc&..." -> "abc" into out. */
static void query_value(const char *topic, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = strstr(topic, key);
    if (p == nullptr) {
        return;
    }
    p += strlen(key);
    size_t n = strcspn(p, "&");
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Desired properties (full twin or patch): apply in the core, report back. */
static void handle_twin(logger_t *lg, const char *json, size_t len)
{
    static uint32_t s_rid;
    twin_desired_t d;
    char reported[LOGGER_REPORTED_MAX], topic[80];
    if (!twin_parse(json, len, &d)) {
        Serial.printf("EVT TWIN_BAD\r\n");
        return;
    }
    int n = logger_apply_twin(lg, &d, reported, sizeof reported);
    if (n < 0) {
        return;
    }
    snprintf(topic, sizeof topic, "$iothub/twin/PATCH/properties/reported/?$rid=r%lu",
             (unsigned long)++s_rid);
    esp_mqtt_client_publish(s_mqtt, topic, reported, n, 0, 0);
}

/* $iothub/methods/POST/<name>/?$rid=<rid> -> run it, reply on methods/res. */
static void handle_method(logger_t *lg, const char *topic)
{
    char name[24], rid[24], reply[96], res_topic[80];
    const char *p = topic + strlen(T_METHOD);
    size_t n = strcspn(p, "/");
    if (n >= sizeof name) {
        n = sizeof name - 1;
    }
    memcpy(name, p, n);
    name[n] = '\0';
    query_value(topic, "$rid=", rid, sizeof rid);

    int status;
    if (strcmp(name, "flush") == 0) {
        status = logger_flush(lg, reply, sizeof reply);
    } else {
        status = 404;
        snprintf(reply, sizeof reply, "{\"error\":\"unknown method\"}");
    }
    snprintf(res_topic, sizeof res_topic, "$iothub/methods/res/%d/?$rid=%s", status, rid);
    esp_mqtt_client_publish(s_mqtt, res_topic, reply, 0, 0, 0);
    Serial.printf("EVT METHOD %s %d\r\n", name, status);
}

static void handle_data(logger_t *lg, const char *topic, const char *data, size_t len)
{
    if (starts_with(topic, T_TWIN_DESIRED)) {
        handle_twin(lg, data, len);
    } else if (starts_with(topic, T_TWIN_RES)) {
        int status = atoi(topic + strlen(T_TWIN_RES));
        if (status == 200 && len > 0) {
            handle_twin(lg, data, len);       /* the full twin we asked for on connect */
        } else if (status < 200 || status >= 300) {
            Serial.printf("EVT NET_ERR twin=%d\r\n", status);
        }                                     /* 204: reported properties accepted */
    } else if (starts_with(topic, T_METHOD)) {
        handle_method(lg, topic);
    }
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
        } else if (m.kind == cloud_msg_t::MSG_DATA) {
            CLOUD_STAGE(STAGE_DATA);
            handle_data(lg, m.buf, m.buf + m.a + 1, (size_t)m.b);
            free(m.buf);
            CLOUD_STAGE(STAGE_QUEUE);
        } else {
            Serial.printf("EVT NET_ERR type=%d connack=%d tls=0x%x\r\n", m.a, m.b, (unsigned)m.c);
        }
    }

    CLOUD_STAGE(STAGE_BOOT_ID);
    settle_boot_id(lg);
    report_tasks();
}

#endif /* COLDTRACE_CLOUD */

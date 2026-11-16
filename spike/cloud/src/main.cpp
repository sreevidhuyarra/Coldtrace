// ColdTrace Block 2 spike (throwaway): can the Wokwi ESP32, inside the VS Code
// extension, reach Azure IoT Hub?
//   STEP 1  join Wokwi-GUEST
//   STEP 2  DNS + TCP to <hub>:8883            (NTP tried too, informational)
//   STEP 3  MQTT over TLS with a SAS token, publish QoS 1 readings, get PUBACKs
// Each step prints PASS or FAIL with the simulated time. Watch the cloud side with
//   az iot hub monitor-events -n <hub> -d <device>
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "mqtt_client.h"   // ESP-IDF esp-mqtt (reports QoS 1 acknowledgements)
#include "azure_ca.h"
#include "secrets.h"       // HUB_HOST, DEVICE_ID, SAS_TOKEN (not committed)

static esp_mqtt_client_handle_t mqtt;
static volatile bool connected = false;
static volatile int acked = 0;
static int sent = 0;
static uint32_t seq = 0;
static char topic[128];

static void say(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Serial.printf("[%7lu ms] %s\r\n", (unsigned long)millis(), buf);
}

static void on_mqtt(void *, esp_event_base_t, int32_t id, void *data) {
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      connected = true;
      say("STEP 3a PASS  MQTT/TLS connected, SAS token accepted");
      break;
    case MQTT_EVENT_DISCONNECTED:
      connected = false;
      say("MQTT disconnected");
      break;
    case MQTT_EVENT_PUBLISHED:
      acked++;
      say("STEP 3b PASS  PUBACK for msg_id=%d (%d/%d acknowledged)", e->msg_id, acked, sent);
      break;
    case MQTT_EVENT_ERROR:
      say("MQTT ERROR type=%d connack=%d tls_err=0x%x tls_stack=0x%x cert_flags=0x%x errno=%d",
          e->error_handle->error_type, e->error_handle->connect_return_code,
          e->error_handle->esp_tls_last_esp_err, e->error_handle->esp_tls_stack_err,
          e->error_handle->esp_tls_cert_verify_flags, e->error_handle->esp_transport_sock_errno);
      break;
    default:
      break;
  }
}

static bool step1_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin("Wokwi-GUEST", "", 6);   // channel 6 skips the scan in Wokwi
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 30000) { say("STEP 1 FAIL  no Wi-Fi after 30 s (status=%d)", WiFi.status()); return false; }
    delay(100);
  }
  say("STEP 1 PASS  Wi-Fi up, IP %s, took %lu ms", WiFi.localIP().toString().c_str(),
      (unsigned long)(millis() - t0));
  return true;
}

static bool step2_reach() {
  IPAddress ip;
  if (!WiFi.hostByName(HUB_HOST, ip)) { say("STEP 2 FAIL  DNS lookup of %s failed", HUB_HOST); return false; }
  say("STEP 2a PASS  %s -> %s", HUB_HOST, ip.toString().c_str());
  WiFiClient tcp;
  uint32_t t0 = millis();
  if (!tcp.connect(ip, 8883)) { say("STEP 2 FAIL  TCP connect to port 8883 refused or timed out"); return false; }
  say("STEP 2b PASS  TCP connect to port 8883 in %lu ms", (unsigned long)(millis() - t0));
  tcp.stop();

  configTime(0, 0, "pool.ntp.org", "time.google.com");   // informational: is UDP/NTP allowed?
  struct tm tm;
  if (getLocalTime(&tm, 10000)) say("NTP  OK    UTC %04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                                     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  else say("NTP  none  no time after 10 s (only matters for display timestamps)");
  return true;
}

static void step3_mqtt() {
  static char uri[160], user[200];
  snprintf(uri, sizeof uri, "mqtts://%s:8883", HUB_HOST);
  snprintf(user, sizeof user, "%s/%s/?api-version=2021-04-12", HUB_HOST, DEVICE_ID);
  snprintf(topic, sizeof topic, "devices/%s/messages/events/", DEVICE_ID);

  esp_mqtt_client_config_t cfg = {};
  cfg.uri = uri;
  cfg.client_id = DEVICE_ID;
  cfg.username = user;
  cfg.password = SAS_TOKEN;
  cfg.cert_pem = AZURE_ROOT_CA;
  cfg.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  cfg.keepalive = 60;
  mqtt = esp_mqtt_client_init(&cfg);
  esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, on_mqtt, nullptr);
  say("STEP 3  connecting to %s as %s", uri, DEVICE_ID);
  esp_mqtt_client_start(mqtt);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  say("ColdTrace cloud spike, fw built " __DATE__ " " __TIME__);
  if (!step1_wifi() || !step2_reach()) { say("SPIKE RESULT: NO-GO"); return; }
  step3_mqtt();
}

void loop() {
  static uint32_t last = 0;
  static bool reported = false;
  if (connected && sent < 5 && millis() - last >= 5000) {
    last = millis();
    char json[96];
    seq++;
    snprintf(json, sizeof json, "{\"dev\":\"%s\",\"seq\":%lu,\"temp\":24.%02lu}", DEVICE_ID,
             (unsigned long)seq, (unsigned long)(seq % 100));
    int id = esp_mqtt_client_publish(mqtt, topic, json, 0, 1, 0);
    sent++;
    say("publish seq=%lu msg_id=%d  %s", (unsigned long)seq, id, json);
  }
  if (!reported && acked >= 5) {
    reported = true;
    say("SPIKE RESULT: GO  (5 readings sent at QoS 1, 5 acknowledged)");
  }
  delay(10);
}

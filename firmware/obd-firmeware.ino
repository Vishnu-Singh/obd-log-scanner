// OBD raw logger for ESP32 (C3 prototype, C6 later) + SN65HVD230
// Captures raw CAN frames and uploads them in JSON batches to the server.
// No decoding happens here: the device only records what's on the bus.
//
// Board: ESP32C3 Dev Module (Arduino-ESP32 core 3.x), "USB CDC On Boot" = Enabled
// Also runs on classic ESP32 / ESP32-C6 with only the pin settings changed.

#include <WiFi.h>
#include <HTTPClient.h>
#include "driver/twai.h"
#include "esp_timer.h"
#include "esp_random.h"

// ---------------- Configuration ----------------
const char* WIFI_SSID = "your-hotspot-name";
const char* WIFI_PASS = "your-hotspot-password";
const char* API_URL   = "http://192.168.43.100:8000/api/v1/frames";
const char* API_KEY   = "change-me";
const char* DEVICE_ID = "ecosport-2014-c3";

// ESP32-C3: GPIO4/5 are safe. Avoid 2, 8, 9 (strapping) and 20/21 (serial).
#define CAN_TX_PIN GPIO_NUM_5   // -> SN65HVD230 CTX
#define CAN_RX_PIN GPIO_NUM_4   // -> SN65HVD230 CRX

// Some ESP32-C3 SuperMini boards can't connect to Wi-Fi at full TX power.
// If it never connects, set this to true.
const bool WIFI_LOW_TX_POWER = false;

// MODE_SELFTEST: bench test without a car. The chip sends frames to itself
//                (no ACK needed) including fake ECU replies, to test the
//                whole pipeline up to the server.
// MODE_SNIFF:    listen-only, never transmits, records all bus traffic.
// MODE_POLL:     sends standard OBD-II requests to 0x7DF and records replies.
enum Mode { MODE_SELFTEST, MODE_SNIFF, MODE_POLL };
const Mode RUN_MODE = MODE_SELFTEST;

// Bus index sent with every frame. 0 = HS-CAN (500k), 1 = MS-CAN (125k, C6 later).
const uint8_t BUS_ID = 0;

// In POLL mode, record only diagnostic IDs (0x7DF, 0x7E0-0x7EF) unless this is true.
const bool POLL_CAPTURE_ALL_TRAFFIC = false;

const uint32_t UPLOAD_INTERVAL_MS = 1000;
const uint16_t BATCH_MAX          = 300;   // frames per HTTP request
const uint16_t QUEUE_LEN          = 1200;  // frames buffered while offline
const uint32_t REQUEST_GAP_MS     = 40;    // gap between OBD requests
const uint32_t CYCLE_GAP_MS       = 200;   // gap between polling cycles

// OBD-II requests. Raw request bytes only; the server decodes responses.
// everyN: send on every Nth cycle (1 = every cycle).
struct Req { uint8_t len; uint8_t bytes[7]; uint16_t everyN; };
const Req REQUESTS[] = {
  {2, {0x01, 0x00}, 500},  // supported PIDs 01-20
  {2, {0x01, 0x20}, 500},  // supported PIDs 21-40
  {2, {0x01, 0x40}, 500},  // supported PIDs 41-60
  {2, {0x01, 0x0C}, 1},    // engine RPM
  {2, {0x01, 0x0D}, 1},    // vehicle speed
  {2, {0x01, 0x04}, 1},    // engine load
  {2, {0x01, 0x0B}, 1},    // intake manifold pressure (boost)
  {2, {0x01, 0x11}, 1},    // throttle position
  {2, {0x01, 0x06}, 1},    // short-term fuel trim bank 1
  {2, {0x01, 0x07}, 1},    // long-term fuel trim bank 1
  {2, {0x01, 0x34}, 1},    // O2 sensor 1 wideband (lambda + current)
  {2, {0x01, 0x15}, 1},    // O2 sensor 2 narrowband voltage (downstream)
  {2, {0x01, 0x05}, 10},   // coolant temperature
  {2, {0x01, 0x3C}, 10},   // catalyst temperature B1S1
  {1, {0x03},       100},  // stored DTCs
  {1, {0x07},       100},  // pending DTCs
  {2, {0x06, 0x21}, 100},  // mode 06: catalyst monitor bank 1
};
// ------------------------------------------------

// Pin tasks to core 1 on dual-core chips, core 0 on single-core (C3/C6).
#if CONFIG_FREERTOS_UNICORE || SOC_CPU_CORES_NUM == 1
  #define APP_CORE 0
#else
  #define APP_CORE 1
#endif

struct Frame {
  uint64_t t_us;    // device uptime in microseconds
  uint32_t id;
  uint8_t  ext;     // 1 = 29-bit ID
  uint8_t  dir;     // 0 = received, 1 = transmitted by us
  uint8_t  dlc;
  uint8_t  data[8];
};

static QueueHandle_t frameQueue;
static volatile uint32_t droppedFrames = 0;
static char bootId[9];

static Frame pending[BATCH_MAX];
static uint16_t pendingCount = 0;

static void enqueue(const Frame& f) {
  if (xQueueSend(frameQueue, &f, 0) != pdTRUE) droppedFrames++;
}

static bool isDiagId(uint32_t id, bool ext) {
  return !ext && (id == 0x7DF || (id >= 0x7E0 && id <= 0x7EF));
}

static bool sendFrame(uint32_t id, const uint8_t* data, uint8_t len) {
  if (RUN_MODE == MODE_SNIFF) return false;  // never transmit in sniff mode
  twai_message_t m = {};
  m.identifier = id;
  m.extd = 0;
  m.data_length_code = len;
  memcpy(m.data, data, len);
  // In self-test the chip receives its own frames, so the RX task logs them.
  if (RUN_MODE == MODE_SELFTEST) m.self = 1;
  if (twai_transmit(&m, pdMS_TO_TICKS(20)) != ESP_OK) return false;

  if (RUN_MODE != MODE_SELFTEST) {
    Frame f = {};
    f.t_us = esp_timer_get_time();
    f.id = id; f.ext = 0; f.dir = 1; f.dlc = len;
    memcpy(f.data, data, len);
    enqueue(f);
  }
  return true;
}

// Reads every frame from the bus and queues it.
static void canRxTask(void*) {
  twai_message_t m;
  for (;;) {
    if (twai_receive(&m, pdMS_TO_TICKS(100)) != ESP_OK) continue;

    bool ext = m.extd;
    bool keep = RUN_MODE != MODE_POLL || POLL_CAPTURE_ALL_TRAFFIC || isDiagId(m.identifier, ext);
    if (keep) {
      Frame f = {};
      f.t_us = esp_timer_get_time();
      f.id = m.identifier; f.ext = ext;
      // In self-test, our own requests come back through RX: mark them as tx.
      f.dir = (RUN_MODE == MODE_SELFTEST && m.identifier == 0x7DF) ? 1 : 0;
      f.dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
      memcpy(f.data, m.data, f.dlc);
      enqueue(f);
    }

    // ISO-TP: when an ECU starts a multi-frame reply (DTC lists, mode 06),
    // send a flow-control frame so it sends the rest.
    if (RUN_MODE == MODE_POLL && !ext &&
        m.identifier >= 0x7E8 && m.identifier <= 0x7EF &&
        m.data_length_code > 0 && (m.data[0] & 0xF0) == 0x10) {
      const uint8_t fc[8] = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};
      sendFrame(m.identifier - 8, fc, 8);
    }
  }
}

static void recoverBusIfNeeded() {
  twai_status_info_t s;
  if (twai_get_status_info(&s) != ESP_OK) return;
  if (s.state == TWAI_STATE_BUS_OFF) {
    twai_initiate_recovery();
  } else if (s.state == TWAI_STATE_STOPPED) {
    twai_start();
  }
}

// Self-test only: pretend to be the engine computer answering a mode 01 request.
static void sendFakeReply(uint8_t pid, uint32_t cycle) {
  uint8_t d[8] = {0};
  if (pid == 0x0C) {                     // RPM = (256A + B) / 4, sweeps 800-2800
    uint16_t raw = (800 + (cycle * 50) % 2000) * 4;
    d[0] = 0x04; d[1] = 0x41; d[2] = pid; d[3] = raw >> 8; d[4] = raw & 0xFF;
  } else if (pid == 0x15) {              // O2S2: steady ~0.68 V, B = 0xFF
    d[0] = 0x04; d[1] = 0x41; d[2] = pid; d[3] = 136; d[4] = 0xFF;
  } else {
    d[0] = 0x03; d[1] = 0x41; d[2] = pid; d[3] = cycle & 0xFF;
  }
  sendFrame(0x7E8, d, 8);
}

// Sends OBD-II requests in a loop.
static void pollTask(void*) {
  uint32_t cycle = 0;
  for (;;) {
    recoverBusIfNeeded();
    for (const Req& r : REQUESTS) {
      if (cycle % r.everyN != 0) continue;
      uint8_t d[8] = {0};
      d[0] = r.len;                   // ISO-TP single frame: length byte
      memcpy(d + 1, r.bytes, r.len);
      sendFrame(0x7DF, d, 8);
      if (RUN_MODE == MODE_SELFTEST && r.bytes[0] == 0x01) {
        sendFakeReply(r.bytes[1], cycle);
      }
      vTaskDelay(pdMS_TO_TICKS(REQUEST_GAP_MS));
    }
    cycle++;
    vTaskDelay(pdMS_TO_TICKS(CYCLE_GAP_MS));
  }
}

static void appendHex(String& s, const uint8_t* d, uint8_t n) {
  static const char* hex = "0123456789ABCDEF";
  for (uint8_t i = 0; i < n; i++) {
    s += hex[d[i] >> 4];
    s += hex[d[i] & 0x0F];
  }
}

static bool uploadPending() {
  if (pendingCount == 0) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  String body;
  body.reserve(pendingCount * 80 + 200);
  char buf[112];
  snprintf(buf, sizeof(buf),
           "{\"device_id\":\"%s\",\"boot_id\":\"%s\",\"sent_us\":%llu,\"dropped\":%lu,\"frames\":[",
           DEVICE_ID, bootId, (unsigned long long)esp_timer_get_time(), (unsigned long)droppedFrames);
  body += buf;

  for (uint16_t i = 0; i < pendingCount; i++) {
    const Frame& f = pending[i];
    snprintf(buf, sizeof(buf), "%s{\"t\":%llu,\"bus\":%u,\"id\":%lu,\"ext\":%u,\"dir\":%u,\"data\":\"",
             i ? "," : "", (unsigned long long)f.t_us, BUS_ID, (unsigned long)f.id, f.ext, f.dir);
    body += buf;
    appendHex(body, f.data, f.dlc);
    body += "\"}";
  }
  body += "]}";

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(API_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  int code = http.POST(body);
  http.end();

  if (code >= 200 && code < 300) {
    Serial.printf("Uploaded %u frames (dropped so far: %lu)\n", pendingCount, (unsigned long)droppedFrames);
    pendingCount = 0;
    return true;
  }
  Serial.printf("Upload failed: HTTP %d\n", code);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // give USB serial time to connect on C3
  snprintf(bootId, sizeof(bootId), "%08lx", (unsigned long)esp_random());

  frameQueue = xQueueCreate(QUEUE_LEN, sizeof(Frame));

  twai_mode_t mode = RUN_MODE == MODE_SNIFF    ? TWAI_MODE_LISTEN_ONLY
                   : RUN_MODE == MODE_SELFTEST ? TWAI_MODE_NO_ACK
                                               : TWAI_MODE_NORMAL;
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, mode);
  g.rx_queue_len = 64;
  g.tx_queue_len = 16;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();   // Ford HS-CAN
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g, &t, &f) != ESP_OK || twai_start() != ESP_OK) {
    Serial.println("CAN init failed - check pins");
    while (true) delay(1000);
  }
  const char* modeName = RUN_MODE == MODE_SNIFF ? "SNIFF" : RUN_MODE == MODE_SELFTEST ? "SELFTEST" : "POLL";
  Serial.printf("CAN started, mode=%s, boot=%s\n", modeName, bootId);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  if (WIFI_LOW_TX_POWER) WiFi.setTxPower(WIFI_POWER_8_5dBm);

  xTaskCreatePinnedToCore(canRxTask, "can_rx", 4096, nullptr, 5, nullptr, APP_CORE);
  if (RUN_MODE != MODE_SNIFF) {
    xTaskCreatePinnedToCore(pollTask, "obd_poll", 4096, nullptr, 4, nullptr, APP_CORE);
  }
}

void loop() {
  static uint32_t lastUpload = 0;
  static wl_status_t lastWifi = WL_IDLE_STATUS;

  wl_status_t w = WiFi.status();
  if (w != lastWifi) {
    lastWifi = w;
    if (w == WL_CONNECTED) Serial.printf("Wi-Fi connected, IP %s\n", WiFi.localIP().toString().c_str());
    else Serial.println("Wi-Fi not connected");
  }

  // Fill the batch from the queue (only when the previous batch was sent).
  while (pendingCount < BATCH_MAX &&
         xQueueReceive(frameQueue, &pending[pendingCount], 0) == pdTRUE) {
    pendingCount++;
  }

  bool full = pendingCount >= BATCH_MAX;
  if (full || millis() - lastUpload >= UPLOAD_INTERVAL_MS) {
    lastUpload = millis();
    uploadPending();
  }
  delay(10);
}

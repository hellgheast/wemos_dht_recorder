#include "transport.h"
#include "config.h"
#include "secrets.h"

#include <ArduinoJson.h>
#include <PacketSerial.h>
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <base64.hpp>

static PacketSerial packetSerial;
static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

static uint32_t packetCounter = 0;
static unsigned long lastWifiAttemptMs = 0;
static unsigned long lastMqttAttemptMs = 0;

static const size_t WIFI_NETWORK_COUNT = sizeof(WIFI_SSID_table) / sizeof(WIFI_SSID_table[0]);
static size_t wifiNetworkIndex = 0;

static void wifiTryNextNetwork() {
  const char *ssid = WIFI_SSID_table[wifiNetworkIndex];
  const char *password = WIFI_PASSWORD_table[wifiNetworkIndex];
  Serial.print(F("WiFi: trying "));
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  wifiNetworkIndex = (wifiNetworkIndex + 1) % WIFI_NETWORK_COUNT;
}

// Chunked "get" file-transfer state, advanced from transportLoop() so a
// transfer never blocks sampling, display, or the Serial/COBS path.
static bool transferActive = false;
static String transferFilename;
static uint32_t transferTotalChunks = 0;
static uint32_t transferSeq = 0;
static unsigned long lastChunkSentMs = 0;

static void onPacketReceived(const uint8_t *buffer, size_t size) {
  // This device only transmits; inbound packets are not expected.
  (void)buffer;
  (void)size;
}

static void publishError(const String &cmd, const String &file, const char *error) {
  JsonDocument resp;
  resp["cmd"] = cmd;
  resp["file"] = file;
  resp["error"] = error;
  char buf[128];
  size_t len = serializeJson(resp, buf, sizeof(buf));
  mqttClient.publish(MQTT_RESP_TOPIC, (const uint8_t *)buf, len, false);
}

static void sendFileList() {
  FileInfo files[MAX_LISTED_FILES];
  int count = storageListFiles(files, MAX_LISTED_FILES);

  const int perPage = 6;
  int pages = count == 0 ? 1 : (count + perPage - 1) / perPage;

  for (int p = 0; p < pages; p++) {
    JsonDocument resp;
    resp["cmd"] = "list";
    resp["part"] = p;
    resp["parts"] = pages;
    JsonArray arr = resp["files"].to<JsonArray>();
    for (int i = p * perPage; i < count && i < (p + 1) * perPage; i++) {
      JsonObject fo = arr.add<JsonObject>();
      fo["name"] = files[i].name;
      fo["size"] = files[i].size;
    }
    char buf[256];
    size_t len = serializeJson(resp, buf, sizeof(buf));
    mqttClient.publish(MQTT_RESP_TOPIC, (const uint8_t *)buf, len, false);
  }
}

static void startFileTransfer(const String &filename) {
  uint32_t size = 0;
  if (!storageOpenForRead(filename, &size)) {
    publishError("get", filename, "not_found");
    return;
  }
  transferFilename = filename;
  transferTotalChunks = (size + MQTT_FILE_CHUNK_SIZE - 1) / MQTT_FILE_CHUNK_SIZE;
  if (transferTotalChunks == 0) {
    transferTotalChunks = 1;  // still send one (empty) chunk for a zero-length file
  }
  transferSeq = 0;
  transferActive = true;
  lastChunkSentMs = 0;  // send the first chunk on the next transportLoop() call
}

static void sendNextChunk() {
  uint8_t raw[MQTT_FILE_CHUNK_SIZE];
  size_t n = storageReadNextChunk(raw, sizeof(raw));

  unsigned int b64Len = encode_base64_length(n);
  char b64Buf[200];
  if (n > 0) {
    encode_base64(raw, n, (unsigned char *)b64Buf);
  }
  b64Buf[b64Len] = '\0';

  JsonDocument resp;
  resp["cmd"] = "get";
  resp["file"] = transferFilename;
  resp["seq"] = transferSeq;
  resp["total"] = transferTotalChunks;
  resp["data"] = b64Buf;

  char jsonBuf[320];
  size_t len = serializeJson(resp, jsonBuf, sizeof(jsonBuf));
  mqttClient.publish(MQTT_RESP_TOPIC, (const uint8_t *)jsonBuf, len, false);

  transferSeq++;
  if (transferSeq >= transferTotalChunks) {
    transferActive = false;
    storageCloseRead();
  }
}

static void handleDeleteCommand(const String &filename) {
  if (transferActive && transferFilename == filename) {
    // Don't delete a file while we're still mid-transfer reading it.
    transferActive = false;
    storageCloseRead();
  }
  bool ok = storageDeleteFile(filename);

  JsonDocument resp;
  resp["cmd"] = "delete";
  resp["file"] = filename;
  resp["ok"] = ok;
  char buf[96];
  size_t len = serializeJson(resp, buf, sizeof(buf));
  mqttClient.publish(MQTT_RESP_TOPIC, (const uint8_t *)buf, len, false);
}

static void onMqttMessage(char *topic, uint8_t *payload, unsigned int length) {
  (void)topic;
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) != DeserializationError::Ok) {
    return;
  }
  const char *cmd = doc["cmd"] | "";
  const char *file = doc["file"] | "";
  String name = file;
  if (strlen(file) > 0 && !name.startsWith("/")) {
    name = "/" + name;
  }

  if (strcmp(cmd, "list") == 0) {
    sendFileList();
  } else if (strcmp(cmd, "get") == 0) {
    if (strlen(file) > 0) {
      startFileTransfer(name);
    }
  } else if (strcmp(cmd, "delete") == 0) {
    if (strlen(file) > 0) {
      handleDeleteCommand(name);
    }
  }
}

void transportBegin() {
  packetSerial.begin(115200);  // owns Serial.begin() internally
  packetSerial.setPacketHandler(&onPacketReceived);

  // PubSubClient.cpp is compiled as its own translation unit, so a
  // #define MQTT_MAX_PACKET_SIZE in this file never reaches it — the actual
  // buffer must be grown at runtime via setBufferSize() instead.
  mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);

  WiFi.mode(WIFI_STA);
  wifiTryNextNetwork();
  lastWifiAttemptMs = millis();
}

void transportLoop() {
  packetSerial.update();

  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastWifiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
      lastWifiAttemptMs = now;
      wifiTryNextNetwork();
    }
  } else if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttAttemptMs >= MQTT_RETRY_INTERVAL_MS) {
      lastMqttAttemptMs = now;
      String clientId = String(MQTT_CLIENT_ID_PREFIX) + String(ESP.getChipId(), HEX);
      bool connected = (strlen(MQTT_USER) > 0)
                            ? mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)
                            : mqttClient.connect(clientId.c_str());
      if (connected) {
        mqttClient.subscribe(MQTT_CMD_TOPIC);
      }
    }
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
  } else if (transferActive) {
    // Lost the connection mid-transfer; abandon it rather than keep reading.
    transferActive = false;
    storageCloseRead();
  }

  if (transferActive) {
    unsigned long now = millis();
    if (now - lastChunkSentMs >= FILE_CHUNK_SEND_INTERVAL_MS) {
      lastChunkSentMs = now;
      sendNextChunk();
    }
  }
}

bool transportIsMqttConnected() {
  return mqttClient.connected();
}

void transportSend(const SensorRecord &rec) {
  JsonDocument doc;
  doc.add(rec.timestamp);
  doc.add(rec.tempCx100);
  doc.add(rec.humidityX100);

  uint8_t msgpackBuf[32];
  size_t msgpackLen = serializeMsgPack(doc, msgpackBuf, sizeof(msgpackBuf));


  // Opportunistically also publish over MQTT if currently connected.
  if (mqttClient.connected()) {
    unsigned int b64Len = encode_base64_length(msgpackLen);
    char b64Buf[64];
    if (b64Len < sizeof(b64Buf)) {
      encode_base64(msgpackBuf, msgpackLen, (unsigned char *)b64Buf);
      b64Buf[b64Len] = '\0';

      JsonDocument envelope;
      envelope["id"] = packetCounter;
      envelope["payload"] = b64Buf;

      char jsonBuf[160];
      size_t jsonLen = serializeJson(envelope, jsonBuf, sizeof(jsonBuf));
      mqttClient.publish(MQTT_TOPIC, (const uint8_t *)jsonBuf, jsonLen, false);
    }
  }

  packetCounter++;
}

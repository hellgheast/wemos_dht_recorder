#include "config.h"
#include "storage.h"
#include "transport.h"

#include "DHT.h"
#include <Wire.h>
#include "SSD1306Wire.h"
#include "RTClib.h"
#include <ESP8266WiFi.h>
#include <time.h>
#include <math.h>

DHT dht(DHTPIN, DHTTYPE);
SSD1306Wire display(SCREEN_ADDRESS, SDA, SCL, GEOMETRY_64_48);
RTC_DS1307 rtc;

static struct {
  bool rtcUsable;

} mainState;

static unsigned long lastSampleMs = 0;
static unsigned long lastDisplayMs = 0;
static float lastHumidity = NAN;
static float lastTempC = NAN;

static void drawStatus(float hum, float temp) {
  char buffer[24];
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  snprintf(buffer, sizeof(buffer), "HUM: %.2f", hum);
  display.drawString(0, 0, buffer);

  snprintf(buffer, sizeof(buffer), "TEMP: %.2f", temp);
  display.drawString(0, 10, buffer);

  display.drawString(0, 20, WiFi.status() == WL_CONNECTED ? "WiFi: up" : "WiFi: down");
  display.drawString(0, 30, transportIsMqttConnected() ? "MQTT: up" : "MQTT: down");
  // Draw the time if we got it or not
}

void setup() {
  pinMode(BUILTIN_LED, OUTPUT);
  dht.begin();

  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);

  transportBegin();  // also brings up Serial at 115200

  Wire.begin(RTC_SDA,RTC_SCL);

  if(!rtc.begin()){
    mainState.rtcUsable = false;
    Serial.println("Couldn't find RTC");
    Serial.flush();
  } else {
    mainState.rtcUsable = true;
    Serial.println("Found RTC");
    Serial.flush();
  }

  Serial.println(F("wemos_dht_recorder starting"));

  if (!storageBegin()) {
    Serial.println(F("LittleFS mount failed"));
  }

  // transportBegin() already brought Wi-Fi up (cycling through
  // WIFI_SSID_table / WIFI_PASSWORD_table as needed).
  configTime(0, 0, NTP_SERVER1, NTP_SERVER2);  // UTC, no DST
  // Set timezone for Switzerland
}

void loop() {
  transportLoop();

  unsigned long now = millis();

  if (now - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
    lastDisplayMs = now;
    display.clear();
    drawStatus(lastHumidity, lastTempC);
    display.display();
  }

  if (lastSampleMs == 0 || now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;

    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (isnan(h) || isnan(t)) {
      Serial.println(F("Failed to read from DHT sensor!"));
      return;
    }

    lastHumidity = h;
    lastTempC = t;

    uint32_t nowUnix = (uint32_t)time(nullptr);

    SensorRecord rec;
    rec.timestamp = nowUnix;
    rec.tempCx100 = (int16_t)lround(t * 100.0f);
    rec.humidityX100 = (int16_t)lround(h * 100.0f);

    if (!storageAppend(rec)) {
      Serial.println(F("Failed to append sensor record to LittleFS"));
    }
    storagePruneOldDays(nowUnix, RETENTION_DAYS);

    transportSend(rec);

    Serial.print(F("Logged + sent: t="));
    Serial.print(t);
    Serial.print(F(" h="));
    Serial.println(h);
  }
}

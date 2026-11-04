#pragma once
#include <Arduino.h>
#include "storage.h"

void transportBegin();
void transportLoop();               // call every loop() iteration
bool transportIsMqttConnected();
void transportSend(const SensorRecord &rec);

// Handles "list" / "get" commands received on MQTT_CMD_TOPIC, replying on
// MQTT_RESP_TOPIC. "get" file transfers are chunked and paced from
// transportLoop(), so they never block sampling or display.

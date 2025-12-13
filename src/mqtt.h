#pragma once
#include <Arduino.h>

void mqttSetup();
bool mqttConnect();
bool mqttPublishAlive();
void mqttLoop();         // för framtida SUBs – just nu no-op
void mqttDisconnect();
bool mqttIsConnected();
void mqttLoopFor(uint32_t durationMs);

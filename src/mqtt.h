#pragma once
#include <Arduino.h>

void mqttSetup();
bool mqttConnect();

bool mqttPublishAlive();
bool mqttPublishVersion(bool retain = true);

void mqttLoop();
void mqttLoopFor(uint32_t durationMs);

void mqttDisconnect();
bool mqttIsConnected();

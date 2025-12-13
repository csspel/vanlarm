#pragma once
#include <Arduino.h>

void modemSetupPins();
void modemPowerOn();

bool modemConnectNetwork(uint32_t timeoutMs, uint32_t &elapsedMs, String &err);
void modemDisconnectNetwork();
int  modemGetSignal();

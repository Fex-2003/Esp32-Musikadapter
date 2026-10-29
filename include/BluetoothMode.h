#pragma once
#include <Arduino.h>
namespace bluetooth {
bool begin(const String& name);
void tick();
void pairing();
void togglePlayback();
void nextTrack();
void previousTrack();
bool connected();
String connectedDeviceName();
bool streaming();
bool pairingActive();
uint32_t pairingSeconds();
void beforeRestart();
void shutdown();
void setLocked(bool enabled);
}

#pragma once
#include <Arduino.h>
#include "ButtonLogic.h"
namespace controls {
using namespace buttons;
void begin();
uint16_t poll(bool playlistActive);
bool modeLocked();
bool fullyLocked();
bool anyDown();
void sleepDisplay();
void display(const String& first, const String& second);
void backlight(bool enabled);
bool lcdReady();
}

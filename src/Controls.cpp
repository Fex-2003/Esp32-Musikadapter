#include "Controls.h"
#include "Config.h"
#include <Wire.h>

namespace controls {
static const int pins[] = {cfg::BUTTON_MODE, cfg::BUTTON_UP, cfg::BUTTON_DOWN, cfg::BUTTON_PLAY};
static buttons::Logic keys(cfg::BUTTON_DEBOUNCE_MS, cfg::BUTTON_MODE_HOLD_MS,
                           cfg::BUTTON_PLAY_HOLD_MS, cfg::BUTTON_COMBO_MS, cfg::BUTTON_PLAY_OTHER_HOLD_MS);
static uint8_t readButtons() {
  uint8_t mask=0;
  for (int i=0;i<4;++i) if (digitalRead(pins[i])==LOW) mask|=1<<i;
  return mask;
}
static uint8_t address = 0;
static bool lcdOk = false, backlightOn = true;
static char cached[2][16];
// Uebliches PCF8574-LCD-Backpack: P0=RS, P1=RW, P2=E, P3=Backlight,
// P4..P7=D4..D7. Kein anderer PCF8574-Aufbau wird stillschweigend angenommen.
static bool pulse(uint8_t nibble, bool rs) {
  const uint8_t value = (nibble & 0xf0) | (backlightOn ? 0x08 : 0x00) | (rs ? 1 : 0);
  Wire.beginTransmission(address);
  Wire.write(value); Wire.write(static_cast<uint8_t>(value | 0x04)); Wire.write(value);
  return Wire.endTransmission() == 0;
}
static bool send(uint8_t value, bool rs) {
  bool ok = pulse(value & 0xf0, rs) && pulse(static_cast<uint8_t>(value << 4), rs);
  delayMicroseconds(50);
  if (!ok) lcdOk = false;
  return ok;
}
static bool probe(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }
void begin() {
  pinMode(cfg::PIR, INPUT_PULLDOWN);
  for (int pin : pins) pinMode(pin, INPUT_PULLUP);
  keys.begin(readButtons(), millis());
  Wire.begin(cfg::LCD_SDA, cfg::LCD_SCL, cfg::LCD_I2C_HZ);
  Wire.setTimeOut(20);
  if (cfg::LCD_ADDRESS) {
    if (probe(cfg::LCD_ADDRESS)) address = cfg::LCD_ADDRESS;
  } else {
    // Nur ein LCD ist vorgesehen: erste antwortende Standard-I2C-Adresse verwenden.
    for (uint8_t candidate = 0x03; candidate <= 0x77; ++candidate) {
      if (probe(candidate)) { address = candidate; break; }
    }
  }
  if (!address) { Serial.println("[LCD] Kein I2C-Geraet im Bereich 0x03-0x77; Betrieb ohne LCD."); return; }
  delay(50);
  pulse(0x30, false); delay(5);
  pulse(0x30, false); delayMicroseconds(150);
  pulse(0x30, false); pulse(0x20, false);
  lcdOk = true;
  send(0x28, false); send(0x08, false); send(0x01, false); delay(2);
  send(0x06, false); send(0x0c, false);
  memset(cached, 0, sizeof(cached));
  Serial.printf("[LCD] Backpack 0x%02x, Standard-Pinbelegung\n", address);
  display("Audio-ESP32 V2", "Start ...");
}
uint16_t poll(bool playlistActive) { return keys.poll(readButtons(), millis(), playlistActive); }
bool modeLocked() { return keys.modeLocked(); }
bool fullyLocked() { return keys.fullyLocked(); }
bool anyDown() { return keys.anyDown(); }
void sleepDisplay() { if (lcdOk) send(0x08, false); backlight(false); }
void display(const String& first, const String& second) {
  if (!lcdOk) return;
  const String* lines[] = {&first, &second};
  for (uint8_t row = 0; row < 2; ++row) {
    for (uint8_t col = 0; col < 16; ++col) {
      unsigned char c = col < lines[row]->length() ? (*lines[row])[col] : ' ';
      if (c < 0x20 || c > 0x7e || c == '\\' || c == '~') c = '?';
      if (cached[row][col] == static_cast<char>(c)) continue;
      if (!send(0x80 | (row ? 0x40 : 0x00) | col, false) || !send(c, true)) return;
      cached[row][col] = c;
    }
  }
}
void backlight(bool enabled) {
  if (!lcdOk || enabled == backlightOn) return;
  Wire.beginTransmission(address);
  Wire.write(static_cast<uint8_t>(enabled ? 0x08 : 0x00));
  if (Wire.endTransmission() == 0) backlightOn = enabled;
  else lcdOk = false;
}
bool lcdReady() { return lcdOk; }
}

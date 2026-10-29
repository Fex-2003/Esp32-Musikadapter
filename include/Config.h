#pragma once
#include <stddef.h>
#include <stdint.h>

// Temporäre Laufzeitdiagnose fuer MP3-Uebergaenge. Fuer reale Hoertests bleibt
// sie aus, weil UART-Ausgaben den kurzen Uebergang selbst messbar verzoegern.
#ifndef AUDIO_MP3_TRANSITION_TRACE
#define AUDIO_MP3_TRANSITION_TRACE 0
#endif

// Alle anwenderseitigen Hardware-/Netzwerkeinstellungen an einer Stelle.
namespace cfg {
constexpr char VERSION[] = "2.1.0";
constexpr char AP_SSID[] = "Audio-esp32";
constexpr char AP_PASSWORD[] = "Musik123";
constexpr uint8_t AP_IP[4] = {192, 168, 4, 1};
constexpr char AP_URL[] = "http://192.168.4.1/";
constexpr uint32_t AP_VISIBLE_MS = 60000;
constexpr uint8_t AP_CHANNEL = 1;
constexpr char DEFAULT_BT_NAME[] = "Audio-ESP32-BT";
constexpr char BT_ADMIN_USER[] = "admin";
constexpr char BT_ADMIN_PASSWORD[] = "123-Musik";
constexpr uint32_t PAIRING_MS = 120000;

// Gegenueber V1: SD_CS von 5 auf 4 umstecken (kein Strapping-Pin).
constexpr int SD_CS = 4, SD_MOSI = 23, SD_CLK = 18, SD_MISO = 19;
constexpr uint32_t SD_SPI_HZ = 10000000;
constexpr int I2S_BCK = 27, I2S_LCK = 26, I2S_DOUT = 25;
constexpr int PIR = 32; // gebuendeltes 3.3-V-Signal, INPUT_PULLDOWN
constexpr int LCD_SDA = 21, LCD_SCL = 22;
constexpr uint8_t LCD_ADDRESS = 0; // 0 = I2C 0x03..0x77 scannen; sonst feste Adresse
constexpr uint32_t LCD_I2C_HZ = 100000;
constexpr int BUTTON_MODE = 33; // Taster gegen GND, interner Pull-up
constexpr int BUTTON_UP = 17;   // UP / Vorskip, Taster gegen GND
constexpr int BUTTON_DOWN = 16; // DOWN / Backskip, Taster gegen GND
constexpr int BUTTON_PLAY = 14; // Taster gegen GND, interner Pull-up
constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
constexpr uint32_t BUTTON_MODE_HOLD_MS = 2000;
constexpr uint32_t BUTTON_PLAY_HOLD_MS = 1500;
constexpr uint32_t BUTTON_PLAY_OTHER_HOLD_MS = 3000;
constexpr uint32_t BUTTON_COMBO_MS = 1000;
constexpr uint32_t AUDIO_FADE_MS = 500;
constexpr uint32_t LCD_SCROLL_MS = 500;
constexpr uint32_t BACKLIGHT_INTERACTION_MS = 120000;

constexpr char MOTION_DIR[] = "/Bewegungsmelder";
constexpr char PLAYLIST_DIR[] = "/Playlists";
constexpr char WEIGHTS_PATH[] = "/Bewegungsmelder/warsch.csv";
constexpr char PLAYLIST_CONFIG[] = "config.txt";
constexpr char UPLOAD_TEMP_NAME[] = "._upload.part";
constexpr uint32_t DEFAULT_OFF_DELAY_S = 0, MAX_OFF_DELAY_S = 600;
constexpr uint8_t DEFAULT_SD_VOLUME = 55;
constexpr uint32_t MP3_BUFFER_BYTES = 4096;
constexpr size_t MAX_PLAYLISTS = 32;
constexpr size_t MAX_TRACKS = 512;       // je Ordner, keine rekursiven Unterordner
constexpr size_t MAX_TITLE_LEN = 40;    // Bewegung: ohne .mp3, ASCII
constexpr size_t MAX_PLAYLIST_LEN = 16; // eine LCD-Zeile, portable ASCII-Auswahl
constexpr size_t MAX_FILENAME_BYTES = 160; // Playlist: inkl. .mp3, UTF-8-Bytes
constexpr size_t MAX_FILENAME_UTF16 = 128;
constexpr size_t MAX_PATH_BYTES = 224;
constexpr uint32_t MAX_FILE_BYTES = 256UL * 1024UL * 1024UL;
constexpr float MAX_WEIGHT = 1000.0f;
constexpr uint32_t RETRY_MS = 3000;
constexpr uint32_t AUDIO_TASK_STACK = 6144;
constexpr uint32_t MIN_HEAP_RESERVE = 24000;
}

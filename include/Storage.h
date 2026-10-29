#pragma once
#include <Arduino.h>
#include <SD.h>
#include <vector>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct Track { String file; float weight = 1.0f; };
struct Playlist {
  String name;
  bool shuffle = false;
  bool repeat = false;
  bool hidden = false;
};
namespace storage {
extern std::atomic<bool> ready;
extern SemaphoreHandle_t mutex;
// Reihenfolge aller Locks: Audio-Lock VOR SD-Lock. Nie umgekehrt.
class Guard {
 public:
  Guard() { if (mutex) xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
  ~Guard() { if (mutex) xSemaphoreGiveRecursive(mutex); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};
bool begin();
bool healthy(); // echter Sektorlesetest, kein gecachter cardType()-Wert
void fault(const String& reason);
String lastError();
void end(); // erst nach audio::stop und abgeschlossenen Webzugriffen
String directory(const String& playlist); // leer -> Bewegungsmelder
bool loadTracks(const String& playlist, std::vector<Track>& tracks, String& error);
bool listPlaylists(std::vector<Playlist>& result, String& error);
bool readConfig(Playlist& playlist);
bool saveConfig(const Playlist& playlist);
bool saveWeights(const std::vector<Track>& tracks);
bool createPlaylist(const String& name, String& error);
bool checkMp3(const String& path, String& error);
bool unicodeTest(String& report);
bool utf8Configured();
bool atomicText(const String& path, const String& text);
void recoverText(const String& path);
String leaf(const String& path);
}

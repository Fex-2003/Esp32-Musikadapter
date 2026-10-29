#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <vector>
#include "Storage.h"

namespace app {
enum class Mode : uint8_t { Motion = 0, Playlist = 1, Bluetooth = 2 };
extern Mode mode;
extern Preferences preferences;
extern std::vector<Playlist> playlists;
extern String selectedPlaylist, activePlaylist, currentSong, note, btName;
extern uint32_t offDelaySeconds;
extern uint16_t sleepMinutes;
extern uint8_t sdVolume;
extern bool editBusy, playlistRunning;
extern size_t playlistPosition, playlistLength;
const char* modeName();
int playlistIndex(const String& name);
void requestMode(Mode target);
bool startPlaylist(const String& name, String& error);
void stopPlayback();
void requestStop();
void activity();
void setVolume(uint8_t percent, bool persist);
bool busy();
bool control(const String& command,String& error);
bool selectTrack(const String& playlist, const String& file, bool next, String& error);
String queuedTrackName();
void beginEdit();
void endEdit();
void reloadCatalog();
bool settings(uint32_t delaySeconds, uint8_t volume, const String& bluetoothName, uint16_t sleep, String& error);
void setup();
void tick();
}

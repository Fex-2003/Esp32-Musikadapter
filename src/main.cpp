/* Audio-ESP32 V2 - Modusverwaltung und Wiedergabeplanung.
 * Basis: hochgeladenes V1-Programm. Pinout/Netzwerkwerte: include/Config.h.
 * Hardware vor Upload gemaess docs/PINOUT.md anpassen (SD-CS jetzt GPIO4!).
 */
#include "App.h"
#include "Config.h"
#include "Rules.h"
#include "AudioEngine.h"
#include "BluetoothMode.h"
#include "Controls.h"
#include "WebUi.h"
#include "FileIssues.h"
#include "BtDevices.h"
#include "PlaylistOrder.h"
#include <esp_bt.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

namespace app {
Mode mode = Mode::Motion;
Preferences preferences;
std::vector<Playlist> playlists;
String selectedPlaylist, activePlaylist, currentSong, note, btName;
uint32_t offDelaySeconds = cfg::DEFAULT_OFF_DELAY_S;
uint16_t sleepMinutes = 0;
uint8_t sdVolume = cfg::DEFAULT_SD_VOLUME;
bool editBusy = false, playlistRunning = false;
size_t playlistPosition = 0, playlistLength = 0;
static std::vector<Track> activeTracks;
static std::vector<uint16_t> playOrder;
struct PlayedTrack { uint16_t index; size_t resumePosition; };
static std::vector<PlayedTrack> recentTracks;
static bool activeShuffle = false, activeRepeat = false, motionLoaded = false;
static bool audioOk = false;
static int previousIndex = -1, pendingMode = -1;
static uint32_t nextTry = 0, lastLcd = 0, backlightUntil = 0;
static size_t failedInRow = 0;
static bool currentHadStart = false;
static void noteLcdInteraction();
static uint32_t lastActivity=0, lastSdCheck=0, overlayUntil=0, transitionAt=0;
static String overlay, pendingPlaylist, pendingTrack;
static int queuedTrack = -1;
enum class Transition { None, Stop, Mode, Sleep };
static Transition transition=Transition::None;
static bool sleepRequested=false;
bool busy() { return editBusy || transition!=Transition::None || sleepRequested || pendingMode>=0; }
void activity() { lastActivity=millis(); noteLcdInteraction(); }
void setVolume(uint8_t value,bool persist) {
  sdVolume=value>100?100:value; audio::volume(sdVolume);
  if(persist && preferences.getUChar("sdVolume",255)!=sdVolume && preferences.putUChar("sdVolume",sdVolume)!=1)
    note="Lautstaerke angewendet, aber NVS-Speicherung fehlgeschlagen.";
}
void requestStop() { if(mode==Mode::Playlist && transition==Transition::None) transition=Transition::Stop; }

const char* modeName() {
  return mode == Mode::Motion ? "Bewegungsmelder" : mode == Mode::Playlist ? "Playlist" : "Bluetooth";
}
int playlistIndex(const String& name) {
  for (size_t i = 0; i < playlists.size(); ++i) if (playlists[i].name == name) return static_cast<int>(i);
  return -1;
}
static void releaseTracks() {
  std::vector<Track>().swap(activeTracks);
  std::vector<uint16_t>().swap(playOrder);
  std::vector<PlayedTrack>().swap(recentTracks);
  motionLoaded = false; previousIndex = -1;
}
void stopPlayback() {
  if (mode != Mode::Bluetooth) audio::stop();
  playlistRunning = false; currentSong = ""; activePlaylist = "";
  playlistPosition = playlistLength = 0;
  currentHadStart = false; failedInRow = 0; nextTry = 0;
  if(queuedTrack>=0 && note.startsWith("Als Naechstes vorgemerkt: ")) note="";
  queuedTrack = -1;
}
static void ensureVisibleSelection() {
  int found = playlistIndex(selectedPlaylist);
  if (found >= 0 && !playlists[found].hidden) return;
  selectedPlaylist = "";
  for (const Playlist& p : playlists) if (!p.hidden) { selectedPlaylist = p.name; break; }
}
void reloadCatalog() {
  if (mode == Mode::Bluetooth) return;
  stopPlayback(); releaseTracks();
  String error;
  storage::listPlaylists(playlists, error);
  if (!error.isEmpty()) note = error;
  ensureVisibleSelection();
  if (mode == Mode::Motion) {
    motionLoaded = storage::loadTracks("", activeTracks, error);
    if (!motionLoaded) note = error;
  }
}
void beginEdit() {
  activity();
  editBusy = true; audio::motionGate(false, offDelaySeconds);
  stopPlayback(); releaseTracks();
}
void endEdit() {
  reloadCatalog(); editBusy = false;
  audio::motionGate(mode == Mode::Motion, offDelaySeconds);
  activity(); // auch ein langer synchroner Upload startet die Inaktivitaetsfrist erst hier neu
}
void requestMode(Mode target) {
  if(controls::fullyLocked() || controls::modeLocked() || transition!=Transition::None) { note="Moduswechsel gesperrt."; return; }
  pendingMode = static_cast<int>(target);
}
static bool switchWifiMode(Mode target, String& error) {
  if (target == mode) return true;
  if (preferences.getUChar("mode",255)!=static_cast<uint8_t>(target) && preferences.putUChar("mode", static_cast<uint8_t>(target)) != 1) {
    error = "Betriebsmodus konnte nicht gespeichert werden."; return false;
  }
  stopPlayback(); releaseTracks(); mode = target;
  noteLcdInteraction();
  audio::motionGate(mode == Mode::Motion, offDelaySeconds);
  if (mode == Mode::Motion) {
    motionLoaded = storage::loadTracks("", activeTracks, error);
    if (!motionLoaded) note = error;
  }
  return true;
}
bool startPlaylist(const String& name, String& error) {
  if (busy() || mode == Mode::Bluetooth || controls::fullyLocked()) { error = "Aktuell kein SD-Start moeglich / Tasten gesperrt."; return false; }
  if (mode!=Mode::Playlist && controls::modeLocked()) { error="Modus gesperrt."; return false; }
  if (!audioOk) { error = "Audio-Engine nicht bereit (seriellen Monitor pruefen)."; return false; }
  const int index = playlistIndex(name);
  if (index < 0) { error = "Playlist nicht vorhanden."; return false; }
  // Kopie vor Freigabe anderer Daten; Einstellungen gelten fuer diesen Durchlauf.
  const Playlist chosen = playlists[index];
  if(mode!=Mode::Playlist && audio::state()==audio::State::Running) {
    pendingPlaylist=name; requestMode(Mode::Playlist); return true;
  }
  if (!switchWifiMode(Mode::Playlist, error)) return false;
  stopPlayback(); releaseTracks();
  if (!storage::loadTracks(chosen.name, activeTracks, error)) { releaseTracks(); return false; }
  if (activeTracks.empty()) { error = "Playlist enthaelt keine zulaessigen MP3-Dateien."; return false; }
  activePlaylist = chosen.name; activeShuffle = chosen.shuffle; activeRepeat = chosen.repeat;
  if (!chosen.hidden) {
    selectedPlaylist = chosen.name;
    if (preferences.getString("selected", "") != chosen.name) preferences.putString("selected", chosen.name);
  }
  rules::order(playOrder, activeTracks.size(), activeShuffle, -1, esp_random);
  playlistPosition = 0; playlistLength = activeTracks.size();
  previousIndex = -1; failedInRow = 0; nextTry = 0;
  playlistRunning = true; note = "";
  return true;
}
bool settings(uint32_t delaySeconds, uint8_t volume, const String& bluetoothName, uint16_t sleep, String& error) {
  if (delaySeconds > cfg::MAX_OFF_DELAY_S || volume > 100 || sleep>240 || !rules::btName(bluetoothName.c_str())) {
    error = "Ungueltige Einstellungen."; return false;
  }
  // Nicht bei jedem Statuspoll/Slider-Pixel in NVS schreiben.
  if (delaySeconds != offDelaySeconds && preferences.putUInt("offDelay", delaySeconds) != sizeof(uint32_t)) {
    error = "Nachlaufzeit konnte nicht gespeichert werden."; return false;
  }
  offDelaySeconds = delaySeconds;
  if (preferences.getUChar("sdVolume",255) != volume && preferences.putUChar("sdVolume", volume) != 1) {
    error = "Lautstaerke konnte nicht gespeichert werden."; return false;
  }
  sdVolume = volume;
  if (bluetoothName != btName && preferences.putString("btName", bluetoothName) != bluetoothName.length()) {
    error = "Bluetooth-Name konnte nicht gespeichert werden."; return false;
  }
  btName = bluetoothName;
  if(sleep!=sleepMinutes && preferences.putUShort("sleepMin",sleep)!=sizeof(uint16_t)) {
    error="Inaktivitaetszeit konnte nicht gespeichert werden."; return false;
  }
  sleepMinutes=sleep;
  audio::volume(sdVolume);
  audio::motionGate(mode == Mode::Motion && !editBusy, offDelaySeconds);
  return true;
}
static bool due(uint32_t deadline) { return deadline == 0 || static_cast<int32_t>(millis()-deadline) >= 0; }
static void noteLcdInteraction() {
  backlightUntil = millis() + cfg::BACKLIGHT_INTERACTION_MS;
  controls::backlight(true);
}
static void rememberPlaylistTrack(uint16_t index, size_t resumePosition) {
  recentTracks.push_back({index, resumePosition});
  // Aktueller Titel plus hoechstens fuenf vorherige fuer DOWN-Backskip.
  if (recentTracks.size() > 6) recentTracks.erase(recentTracks.begin());
}
static void returnToPlaylistSelection(const String& message = "", bool automaticEnd = false) {
  const String finished = activePlaylist;
  stopPlayback(); releaseTracks();
  const int index = playlistIndex(finished);
  if (index >= 0 && !playlists[index].hidden) {
    selectedPlaylist = finished;
    if (preferences.getString("selected", "") != finished) preferences.putString("selected", finished);
  }
  if (automaticEnd) noteLcdInteraction();
  if (!message.isEmpty()) note = message;
}
static bool playIndex(size_t index, bool rememberPlaylist = false, size_t resumePosition = 0) {
  String error;
  const String folder = mode == Mode::Motion ? String("") : activePlaylist;
  const String path = storage::directory(folder) + "/" + activeTracks[index].file;
  previousIndex = static_cast<int>(index);
  if(issues::blocked(path)) { ++failedInRow; return false; }
  if (audio::play(path, sdVolume, error)) {
    currentSong = activeTracks[index].file;
    currentHadStart = true; note = "";
    if (rememberPlaylist) rememberPlaylistTrack(static_cast<uint16_t>(index), resumePosition);
    return true;
  }
  currentSong = ""; note = error;
  Serial.printf("[Audio] Fehler %s: %s\n", path.c_str(), error.c_str());
  if(storage::healthy() && audio::fileFailed())
    issues::record(path,error);
  ++failedInRow; nextTry = millis() + cfg::RETRY_MS;
  if (mode == Mode::Motion) activeTracks[index].weight = 0; // nur RAM, CSV unveraendert
  return false;
}
static bool playNextPlaylistTrack() {
  if (queuedTrack >= 0) {
    const uint16_t index = static_cast<uint16_t>(queuedTrack);
    queuedTrack = -1;
    playlist_order::takeRequested(playOrder, playlistPosition, index);
    return playIndex(index, true, playlistPosition);
  }
  if (playlistPosition >= playOrder.size()) {
    if (!activeRepeat) { returnToPlaylistSelection("", true); return false; }
    rules::order(playOrder, activeTracks.size(), activeShuffle, previousIndex, esp_random);
    playlistPosition = 0;
    recentTracks.clear();
  }
  const size_t orderPosition = playlistPosition++;
  return playIndex(playOrder[orderPosition], true, playlistPosition);
}
static void skipPlaylistForward() {
  if (!playlistRunning) return;
  audio::stop(); currentSong = ""; currentHadStart = false;
  failedInRow = 0; nextTry = 0;
  playNextPlaylistTrack();
}
static void skipPlaylistBack() {
  if (!playlistRunning || recentTracks.size() < 2) return;
  recentTracks.pop_back();
  const PlayedTrack target = recentTracks.back();
  playlistPosition = target.resumePosition;
  audio::stop(); currentSong = ""; currentHadStart = false;
  failedInRow = 0; nextTry = 0;
  playIndex(target.index);
}
bool selectTrack(const String& playlist, const String& file, bool next, String& error) {
  if(busy() || controls::fullyLocked() || mode==Mode::Bluetooth || !audioOk) {
    error="Titelwahl derzeit nicht moeglich."; return false;
  }
  if(playlistIndex(playlist)<0 || !rules::musicFile(file.c_str())) {
    error="Playlist oder Dateiname ungueltig."; return false;
  }
  const String path=storage::directory(playlist)+"/"+file;
  if(path.length()>cfg::MAX_PATH_BYTES) { error="Dateipfad zu lang."; return false; }
  if(issues::blocked(path)) { error="Titel ist wegen eines Dateifehlers gesperrt."; return false; }
  if(!storage::ready || !storage::checkMp3(path,error)) {
    if(error.isEmpty()) error="SD-Karte nicht verfuegbar.";
    return false;
  }
  if(next && (mode!=Mode::Playlist || !playlistRunning || activePlaylist!=playlist)) {
    error="Bitte zuerst diese Playlist starten, um einen Titel vorzumerken."; return false;
  }
  if(!next && (mode!=Mode::Playlist || !playlistRunning || activePlaylist!=playlist)) {
    if(!startPlaylist(playlist,error)) return false;
    if(pendingMode>=0) { pendingTrack=file; return true; }
  }
  int index=-1;
  for(size_t i=0;i<activeTracks.size();++i) if(activeTracks[i].file==file) { index=static_cast<int>(i); break; }
  if(index<0) { error="Titel nicht in der aktuellen Playlist gefunden; bitte neu einlesen."; return false; }
  if(next) {
    queuedTrack=index;
    note="Als Naechstes vorgemerkt: "+file;
    return true;
  }
  if(queuedTrack==index) queuedTrack=-1;
  audio::stop(); currentSong=""; currentHadStart=false;
  failedInRow=0; nextTry=0;
  playlist_order::takeRequested(playOrder,playlistPosition,static_cast<uint16_t>(index));
  if(!playIndex(static_cast<size_t>(index),true,playlistPosition)) {
    error=note.isEmpty()?"Titel konnte nicht gestartet werden.":note; return false;
  }
  return true;
}
String queuedTrackName() {
  return playlistRunning && queuedTrack>=0 && static_cast<size_t>(queuedTrack)<activeTracks.size()
    ? activeTracks[queuedTrack].file : String("");
}
static void servicePlayback() {
  // SD-Ausfaelle werden zentral durch serviceSd behandelt.
  if (!audioOk || !storage::ready || editBusy || mode == Mode::Bluetooth) return;
  const audio::State state = audio::state();
  if (state == audio::State::Running) return;
  if (state == audio::State::StorageError) {
    storage::fault(audio::errorMessage()); stopPlayback(); return;
  }
  if (state == audio::State::Ended || state == audio::State::MotionTimeout || state==audio::State::FileError) {
    const bool hadSamples = audio::emittedSamples();
    if(state==audio::State::FileError && !currentSong.isEmpty()) {
      const String path=storage::directory(mode==Mode::Motion?String(""):activePlaylist)+"/"+currentSong;
      issues::record(path,audio::errorMessage());
    }
    audio::stop(); currentSong = "";
    if (currentHadStart && (state == audio::State::Ended || state==audio::State::FileError)) {
      if (!hadSamples || state==audio::State::FileError) {
        ++failedInRow; nextTry = millis() + cfg::RETRY_MS;
        if (mode == Mode::Motion && previousIndex >= 0) activeTracks[previousIndex].weight = 0;
        note = "Titel ohne dekodierbare Audiosamples uebersprungen.";
      } else { failedInRow = 0; nextTry = 0; }
    }
    currentHadStart = false;
  }
  if (!due(nextTry)) return;
  if (mode == Mode::Motion) {
    if (!motionLoaded || !audio::motionWanted() || activeTracks.empty()) return;
    std::vector<double> weights; weights.reserve(activeTracks.size());
    for (const Track& t : activeTracks) weights.push_back(issues::blocked(storage::directory("")+"/"+t.file)?0:t.weight);
    const int next = rules::weightedPick(weights, previousIndex, esp_random());
    if (next >= 0) playIndex(static_cast<size_t>(next));
  } else if (playlistRunning) {
    if (failedInRow >= activeTracks.size()) {
      returnToPlaylistSelection("Kein abspielbarer Titel im Durchlauf."); return;
    }
    playNextPlaylistTrack();
  }
}
bool control(const String& command,String& error) {
  if(busy() || controls::fullyLocked() || mode!=Mode::Playlist || !playlistRunning) { error="Keine aktive, bedienbare Playlist."; return false; }
  if(command=="pause") audio::pause(!audio::pauseRequested());
  else if(command=="stop") requestStop();
  else if(command=="next") skipPlaylistForward();
  else if(command=="back") skipPlaylistBack();
  else { error="Unbekannter Befehl."; return false; }
  return true;
}
static void stepSelection(int direction) {
  if (playlists.empty()) return;
  int i = playlistIndex(selectedPlaylist);
  if (i < 0) i = direction > 0 ? -1 : 0;
  for (size_t tries = 0; tries < playlists.size(); ++tries) {
    i = (i + direction + static_cast<int>(playlists.size())) % static_cast<int>(playlists.size());
    if (!playlists[i].hidden) { selectedPlaylist = playlists[i].name; return; }
  }
  selectedPlaylist = "";
}
static String lcdAscii(const String& source) {
  String result; result.reserve(source.length());
  for (size_t i = 0; i < source.length();) {
    const unsigned char c = static_cast<unsigned char>(source[i]);
    if (c >= 0x20 && c <= 0x7e && c != '\\' && c != '~') {
      result += static_cast<char>(c); ++i;
    } else {
      result += '?'; ++i;
      while (i < source.length() && (static_cast<unsigned char>(source[i]) & 0xc0) == 0x80) ++i;
    }
  }
  return result;
}
static String scrollLine(const String& source) {
  const String text = lcdAscii(source);
  if (text.length() <= 16) return text;
  const String cycle = text + "   " + text;
  const size_t offset = (millis() / cfg::LCD_SCROLL_MS) % (text.length() + 3);
  return cycle.substring(offset, offset + 16);
}
static String songTitle(const String& file) {
  return file.length() > 4 && file[file.length()-4] == '.' ? file.substring(0, file.length()-4) : file;
}
static void updateLcd() {
  if (static_cast<uint32_t>(millis()-lastLcd) < 250) return;
  lastLcd = millis();
  if(transition==Transition::Sleep) { controls::display("Ausschalten ...", "Tasten loslassen"); return; }
  if(!due(overlayUntil)) { controls::display(modeName(),overlay); return; }
  overlayUntil=0;
  if (editBusy) { controls::display(modeName(), "SD-Verwaltung"); return; }
  if(mode!=Mode::Bluetooth && !storage::ready) { controls::display(modeName(),"SD-Kartenfehler"); return; }
  if (mode == Mode::Bluetooth) {
    String second=bluetooth::connectedDeviceName();
    if(bluetooth::pairingActive()) {
      const String pairing="Pairing " + String(bluetooth::pairingSeconds()) + "s";
      second=second.isEmpty()?pairing:pairing+" | "+second;
    }
    controls::display(modeName(),scrollLine(second));
  } else if (mode == Mode::Motion) {
    controls::display(modeName(), "");
  } else {
    const String second = playlistRunning ? songTitle(currentSong) : selectedPlaylist;
    controls::display(modeName(), scrollLine(second));
  }
}
static void updateBacklight() {
  if(due(backlightUntil)) backlightUntil=0;
  const bool activePlaylistAudio = mode == Mode::Playlist && playlistRunning &&
                                   audio::state() == audio::State::Running && !audio::paused();
  controls::backlight(activePlaylistAudio || !due(backlightUntil));
}
void setup() {
  Serial.begin(115200); delay(100);
  Serial.printf("\n=== Audio-ESP32 %s ===\n", cfg::VERSION);
  const esp_sleep_wakeup_cause_t wake=esp_sleep_get_wakeup_cause();
  if(wake==ESP_SLEEP_WAKEUP_EXT0) rtc_gpio_deinit(static_cast<gpio_num_t>(cfg::BUTTON_MODE));
  controls::begin(); // beim Boot gehaltene Tasten werden bis zum Loslassen ignoriert
  noteLcdInteraction();
  if (!preferences.begin("audioesp", false)) Serial.println("[NVS] Einstellungen nicht verfuegbar");
  uint8_t storedMode = preferences.getUChar("mode", 0);
  mode = storedMode <= 2 ? static_cast<Mode>(storedMode) : Mode::Motion;
  offDelaySeconds = preferences.getUInt("offDelay", cfg::DEFAULT_OFF_DELAY_S);
  if (offDelaySeconds > cfg::MAX_OFF_DELAY_S) offDelaySeconds = cfg::DEFAULT_OFF_DELAY_S;
  sdVolume = preferences.getUChar("sdVolume", cfg::DEFAULT_SD_VOLUME);
  if (sdVolume > 100) sdVolume = cfg::DEFAULT_SD_VOLUME;
  sleepMinutes=preferences.getUShort("sleepMin",0); if(sleepMinutes>240) sleepMinutes=0;
  btdevices::begin();
  btName = preferences.getString("btName", cfg::DEFAULT_BT_NAME);
  if (!rules::btName(btName.c_str())) btName = cfg::DEFAULT_BT_NAME;
  selectedPlaylist = preferences.getString("selected", "");
  if (mode == Mode::Bluetooth) {
    if (!bluetooth::begin(btName)) note = "Bluetooth konnte nicht initialisiert werden.";
  } else {
    // Speicherfreigabe ist bis Neustart irreversibel. Deshalb Restart an BT-Grenze.
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
      esp_err_t rc = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
      Serial.printf("[RAM] Bluetooth-Speicher freigeben: %d\n", rc);
    }
    storage::begin();
    issues::begin();
    audioOk = audio::begin();
    if (!audioOk) note = "Audio-Task konnte nicht angelegt werden.";
    reloadCatalog();
    audio::motionGate(mode == Mode::Motion, offDelaySeconds);
    if (!webui::begin()) note = "WLAN-AP konnte nicht gestartet werden.";
  }
  Serial.printf("[Modus] %s | Heap %u Bytes\n", modeName(), ESP.getFreeHeap());
  lastActivity=millis();
  Serial.printf("[Start] Reset=%d, Wakeup=%d, Auto-Sleep=%u min\n",esp_reset_reason(),wake,sleepMinutes);
  updateLcd();
}
static void serviceSd() {
  if(mode==Mode::Bluetooth || editBusy || transition!=Transition::None) return;
  const uint32_t now=millis();
  if(storage::ready) {
    if(uint32_t(now-lastSdCheck)>=5000) { lastSdCheck=now; storage::healthy(); }
    if(!storage::ready) { note=storage::lastError(); stopPlayback(); releaseTracks(); playlists.clear(); }
    else if(audio::state()!=audio::State::Running || audio::paused()) issues::flush();
  } else {
    if(audio::state()!=audio::State::Idle || playlistRunning || motionLoaded || !playlists.empty()) {
      stopPlayback(); releaseTracks(); playlists.clear();
    }
    note=storage::lastError();
    if(uint32_t(now-lastSdCheck)>=10000) {
      lastSdCheck=now;
      if(storage::begin()) {
        issues::begin(); reloadCatalog(); note="SD-Karte wieder bereit. Playlist bei Bedarf neu starten.";
        audio::motionGate(mode==Mode::Motion,offDelaySeconds);
      }
    }
  }
}
static void serviceTransition() {
  static bool started=false;
  if(editBusy) return;
  if(transition==Transition::None) {
    if(sleepRequested) transition=Transition::Sleep;
    else if(pendingMode>=0 && pendingMode!=static_cast<int>(mode)) transition=Transition::Mode;
    else { pendingMode=-1; return; }
  }
  if(!started) {
    const uint8_t savedMode=transition==Transition::Mode?static_cast<uint8_t>(pendingMode):static_cast<uint8_t>(mode);
    if(preferences.getUChar("mode",255)!=savedMode && preferences.putUChar("mode",savedMode)!=1) {
      note="Abschaltung/Moduswechsel abgebrochen: NVS-Schreibfehler.";
      transition=Transition::None; pendingMode=-1; pendingPlaylist=""; pendingTrack=""; sleepRequested=false; activity(); return;
    }
    started=true; transitionAt=millis();
    if(mode==Mode::Bluetooth) bluetooth::beforeRestart();
    else { audio::motionGate(false,offDelaySeconds); audio::fadeStop(); }
  }
  if(mode!=Mode::Bluetooth && audio::stopping()) return;
  if(mode==Mode::Bluetooth && bluetooth::connected() && uint32_t(millis()-transitionAt)<1500) return;
  if(transition==Transition::Sleep) {
    if(controls::anyDown()) return;
    stopPlayback(); issues::flush(true); btdevices::save();
    if(mode==Mode::Bluetooth) bluetooth::shutdown();
    else { webui::end(); storage::end(); }
    controls::sleepDisplay();
    const gpio_num_t pin=static_cast<gpio_num_t>(cfg::BUTTON_MODE);
    rtc_gpio_init(pin); rtc_gpio_set_direction(pin,RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(pin); rtc_gpio_pulldown_dis(pin);
    esp_sleep_enable_ext0_wakeup(pin,0);
    Serial.println("[Sleep] Deep Sleep; MODE weckt auf."); Serial.flush();
    esp_deep_sleep_start();
  }
  const Transition completed=transition;
  transition=Transition::None; started=false;
  if(completed==Transition::Stop) { returnToPlaylistSelection(); return; }
  const Mode next=static_cast<Mode>(pendingMode); pendingMode=-1;
  if(next==Mode::Bluetooth || mode==Mode::Bluetooth) {
    controls::display("Moduswechsel","Neustart ...");
    if(mode==Mode::Bluetooth) bluetooth::shutdown();
    else { stopPlayback(); issues::flush(true); webui::end(); storage::end(); }
    ESP.restart();
  } else {
    String error;
    if(!switchWifiMode(next,error)) note=error;
    if(!pendingPlaylist.isEmpty()) {
      const String name=pendingPlaylist; pendingPlaylist="";
      const String file=pendingTrack; pendingTrack="";
      if(!startPlaylist(name,error)) note=error;
      else if(!file.isEmpty() && !selectTrack(name,file,false,error)) note=error;
    }
  }
}
void tick() {
  const uint16_t events = controls::poll(mode==Mode::Playlist && playlistRunning);
  if(events & controls::Interaction) activity();
  if(events & controls::LockChanged) {
    static bool wasFull=false;
    overlay=controls::fullyLocked()?"Tasten gesperrt":wasFull?"Tasten frei":controls::modeLocked()?"Modus gesperrt":"Modus frei";
    wasFull=controls::fullyLocked(); overlayUntil=millis()+2000;
    bluetooth::setLocked(controls::fullyLocked() || controls::modeLocked()); activity();
  }
  if (!busy()) {
    if(events & controls::ModeLong) sleepRequested=true;
    if (events & controls::Mode) requestMode(static_cast<Mode>((static_cast<uint8_t>(mode)+1)%3));
    else if (mode == Mode::Playlist) {
      if ((events & controls::PlayLong) && playlistRunning) {
        requestStop();
      } else if (playlistRunning) {
        if (events & controls::Play) audio::pause(!audio::pauseRequested());
        if (events & controls::Up) skipPlaylistForward();
        if (events & controls::Down) skipPlaylistBack();
      } else {
        if (events & controls::Up) stepSelection(1);
        if (events & controls::Down) stepSelection(-1);
        if (events & controls::Play) { String error; if (!startPlaylist(selectedPlaylist, error)) note = error; }
      }
    } else if (mode == Mode::Bluetooth) {
      if(events & controls::PlayLong) bluetooth::pairing();
      else if(events & controls::Play) {
        if(bluetooth::connected()) bluetooth::togglePlayback();
        else bluetooth::pairing();
      }
      if(events & controls::Up) bluetooth::nextTrack();
      if(events & controls::Down) bluetooth::previousTrack();
    }
  }
  const bool locked=controls::fullyLocked() || controls::modeLocked();
  const bool playing=mode==Mode::Bluetooth?bluetooth::streaming():
    mode==Mode::Motion?audio::motionWanted() || digitalRead(cfg::PIR)==HIGH:
    audio::state()==audio::State::Running && !audio::paused();
  if(playing || locked || editBusy || controls::anyDown()) lastActivity=millis();
  if(sleepMinutes && !locked && !busy() && uint32_t(millis()-lastActivity)>=uint32_t(sleepMinutes)*60000UL) sleepRequested=true;
  serviceTransition();
  if (mode == Mode::Bluetooth) bluetooth::tick();
  else {
    serviceSd();
    if(!busy()) servicePlayback();
    webui::tick();
  }
  updateLcd();
  updateBacklight();
  delay(1);
}
}
void setup() { app::setup(); }
void loop() { app::tick(); }

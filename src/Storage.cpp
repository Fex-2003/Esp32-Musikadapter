#include "Storage.h"
#include "Config.h"
#include "Rules.h"
#include <SPI.h>
#include <sdkconfig.h>
#include <esp_system.h>
#include <algorithm>

namespace storage {
std::atomic<bool> ready{false};
static String sdError;
SemaphoreHandle_t mutex = nullptr;
void fault(const String& reason) { Guard lock; sdError=reason; ready=false; }
String lastError() { Guard lock; return sdError; }
bool healthy() {
  Guard lock;
  if (!ready) return false;
  uint8_t sector[512];
  if (SD.readRAW(sector,0)) return true;
  fault("SD-Karte antwortet nicht. Automatische Wiederherstellung laeuft."); return false;
}
void end() { Guard lock; SD.end(); ready=false; }
static bool validFile(const String& s, bool motion) {
  return motion ? rules::motionFile(s.c_str()) : rules::musicFile(s.c_str());
}
String leaf(const String& path) {
  const int at = path.lastIndexOf('/');
  return at < 0 ? path : path.substring(at+1);
}
String directory(const String& playlist) {
  return playlist.isEmpty() ? String(cfg::MOTION_DIR) : String(cfg::PLAYLIST_DIR) + "/" + playlist;
}
bool utf8Configured() {
#if defined(CONFIG_FATFS_API_ENCODING_UTF_8) && (defined(CONFIG_FATFS_LFN_STACK) || defined(CONFIG_FATFS_LFN_HEAP))
  return true;
#else
  return false;
#endif
}
static void recover(const String& path) {
  const String temp = path + ".tmp", backup = path + ".bak";
  if (!SD.exists(path)) {
    if (SD.exists(backup)) SD.rename(backup, path);
    else if (SD.exists(temp)) SD.rename(temp, path);
  }
  if (SD.exists(path)) { if(SD.exists(temp)) SD.remove(temp); if(SD.exists(backup)) SD.remove(backup); }
}
void recoverText(const String& path) { Guard lock; if(ready) recover(path); }
bool atomicText(const String& path, const String& text) {
  Guard lock;
  if (!ready) return false;
  recover(path);
  const String temp = path + ".tmp", backup = path + ".bak";
  SD.remove(temp);
  File f = SD.open(temp, FILE_WRITE);
  if (!f) { fault("SD-Konfiguration nicht schreibbar."); return false; }
  const bool ok = f.print(text) == text.length();
  f.flush(); f.close();
  if (!ok) { SD.remove(temp); fault("SD-Schreibfehler / Karte voll."); return false; }
  File verify=SD.open(temp,FILE_READ);
  const bool complete=verify && verify.size()==text.length(); verify.close();
  if(!complete) { fault("SD-Konfiguration nach Schreiben unvollstaendig."); return false; }
  SD.remove(backup);
  const bool existed = SD.exists(path);
  if (existed && !SD.rename(path, backup)) { SD.remove(temp); fault("SD-Backup konnte nicht angelegt werden."); return false; }
  if (!SD.rename(temp, path)) {
    if (existed) SD.rename(backup, path);
    SD.remove(temp); fault("SD-Konfiguration konnte nicht uebernommen werden."); return false;
  }
  SD.remove(backup); return true;
}
bool begin() {
  if (!mutex) mutex = xSemaphoreCreateRecursiveMutex();
  if (!mutex) return false;
  Guard lock;
  SD.end();
  SPI.begin(cfg::SD_CLK, cfg::SD_MISO, cfg::SD_MOSI, cfg::SD_CS);
  ready = SD.begin(cfg::SD_CS, SPI, cfg::SD_SPI_HZ, "/sd", 6);
  if (!ready) { fault("SD-Mount fehlgeschlagen (FAT32 / CS=4 pruefen)"); Serial.println(sdError); return false; }
  if ((!SD.exists(cfg::MOTION_DIR) && !SD.mkdir(cfg::MOTION_DIR)) ||
      (!SD.exists(cfg::PLAYLIST_DIR) && !SD.mkdir(cfg::PLAYLIST_DIR))) {
    fault("SD-Verzeichnisse konnten nicht angelegt werden"); return false;
  }
  SD.remove(String(cfg::MOTION_DIR) + "/" + cfg::UPLOAD_TEMP_NAME);
  Serial.printf("[SD] bereit; API UTF-8+LFN: %s\n", utf8Configured() ? "JA" : "NEIN - Unicode ungesichert");
  sdError="";
  return true;
}
// Begrenztes Lesen vermeidet String-Wachstum bei beschaedigten Konfigurationsdateien.
static bool readLine(File& f, String& line, size_t max) {
  line = ""; bool overflow = false;
  while (f.available()) {
    int c = f.read();
    if (c < 0 || c == '\n') break;
    if (line.length() < max) line += static_cast<char>(c); else overflow = true;
  }
  if (line.endsWith("\r")) line.remove(line.length()-1);
  return !overflow;
}
bool readConfig(Playlist& p) {
  Guard lock;
  p.shuffle = p.repeat = p.hidden = false;
  const String path = directory(p.name) + "/" + cfg::PLAYLIST_CONFIG;
  recover(path);
  File f = SD.open(path, FILE_READ);
  if (!f) return true; // Standardwerte, Datei erst bei Aenderung anlegen.
  if (f.size() > 512) { f.close(); return false; }
  bool ok = true;
  while (f.available()) {
    String line; if (!readLine(f, line, 80)) { ok = false; continue; }
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;
    int sep = line.indexOf('=');
    if (sep < 1) { ok = false; continue; }
    String key = line.substring(0, sep), value = line.substring(sep+1);
    key.trim(); value.trim();
    if (value != "0" && value != "1") { ok = false; continue; }
    if (key == "shuffle") p.shuffle = value == "1";
    else if (key == "repeat") p.repeat = value == "1";
    else if (key == "hidden") p.hidden = value == "1";
    else ok = false;
  }
  f.close(); return ok;
}
bool saveConfig(const Playlist& p) {
  if (!rules::playlistName(p.name.c_str())) return false;
  String text = "shuffle=" + String(p.shuffle ? 1 : 0) + "\nrepeat=" + String(p.repeat ? 1 : 0) +
                "\nhidden=" + String(p.hidden ? 1 : 0) + "\n";
  return atomicText(directory(p.name) + "/" + cfg::PLAYLIST_CONFIG, text);
}
bool listPlaylists(std::vector<Playlist>& result, String& error) {
  result.clear(); error = "";
  if (!ready) { error = "SD-Karte nicht verfuegbar."; return false; }
  File root;
  { Guard lock; root = SD.open(cfg::PLAYLIST_DIR); }
  if (!root || !root.isDirectory()) { error = "Playlists-Verzeichnis nicht lesbar."; healthy(); return false; }
  while (true) {
    String path; bool isDir = false;
    { Guard lock; path = root.getNextFileName(&isDir); }
    if (path.isEmpty()) break;
    String name = leaf(path);
    if (!isDir || name.startsWith(".")) continue;
    if (!rules::playlistName(name.c_str())) {
      Serial.printf("[SD] Ungueltiger LCD-Playlistname ignoriert: %s\n", name.c_str());
      error = "Ungueltige Playlistnamen ignoriert (siehe serieller Monitor / README)."; continue;
    }
    if (result.size() >= cfg::MAX_PLAYLISTS) { error = "Mehr als 32 Playlists: weitere Ordner werden ignoriert."; break; }
    Playlist p; p.name = name;
    if (!readConfig(p)) error = "Mindestens eine config.txt ungueltig; gueltige Felder/Standardwerte verwendet.";
    { Guard lock; SD.remove(path + "/" + cfg::UPLOAD_TEMP_NAME); }
    result.push_back(p);
    delay(1);
  }
  { Guard lock; root.close(); }
  std::sort(result.begin(), result.end(), [](const Playlist& a, const Playlist& b) { return a.name.compareTo(b.name) < 0; });
  return true;
}
bool loadTracks(const String& playlist, std::vector<Track>& tracks, String& error) {
  tracks.clear(); error = "";
  if (!ready) { error = "SD-Karte nicht verfuegbar."; return false; }
  if (!playlist.isEmpty() && !rules::playlistName(playlist.c_str())) { error = "Ungueltige Playlist."; return false; }
  const String dirName = directory(playlist);
  File root;
  { Guard lock; root = SD.open(dirName); }
  if (!root || !root.isDirectory()) { error = "Ordner nicht lesbar."; healthy(); return false; }
  while (true) {
    String path; bool isDir = false;
    { Guard lock; path = root.getNextFileName(&isDir); }
    if (path.isEmpty()) break;
    const String file = leaf(path);
    if (isDir || !rules::mp3Extension(file.c_str()) || file.startsWith(".")) continue;
    if (!validFile(file, playlist.isEmpty()) || path.length() > cfg::MAX_PATH_BYTES) {
      Serial.printf("[SD] Dateiname ignoriert: %s\n", path.c_str()); continue;
    }
    if (tracks.size() >= cfg::MAX_TRACKS) {
      error = "Mehr als 512 gueltige Titel in diesem Ordner. Wiedergabe/Schreibverwaltung gesperrt; am PC aufteilen.";
      { Guard lock; root.close(); }
      return false;
    }
    if (ESP.getFreeHeap() < cfg::MIN_HEAP_RESERVE + 1024) {
      error = "Zu wenig freier RAM fuer weitere Dateinamen.";
      { Guard lock; root.close(); }
      return false;
    }
    Track track; track.file = file; tracks.push_back(track);
    delay(1);
  }
  { Guard lock; root.close(); }
  std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
    return rules::naturalCompare(a.file.c_str(), b.file.c_str()) < 0;
  });
  if (!playlist.isEmpty()) return true;
  Guard lock;
  recover(cfg::WEIGHTS_PATH);
  File f = SD.open(cfg::WEIGHTS_PATH, FILE_READ);
  if (!f) return true;
  if (f.size() > 16384) { f.close(); error = "warsch.csv groesser als 16 KiB; bitte pruefen."; return false; }
  while (f.available()) {
    String line;
    if (!readLine(f, line, 100)) continue;
    if (line.startsWith("\xef\xbb\xbf")) line.remove(0, 3);
    line.trim(); const int sep = line.indexOf(',');
    if (sep < 1) continue;
    String title = line.substring(0, sep), value = line.substring(sep+1);
    title.trim(); value.trim(); double w;
    if (!rules::motionFile((title + ".mp3").c_str()) || !rules::weight(value.c_str(), w)) continue;
    for (Track& t : tracks) {
      if (t.file.substring(0, t.file.length()-4).equalsIgnoreCase(title)) { t.weight = static_cast<float>(w); break; }
    }
  }
  f.close(); return true;
}
bool saveWeights(const std::vector<Track>& tracks) {
  String text; if (!text.reserve(tracks.size()*64+1)) return false;
  for (const Track& t : tracks) {
    if (!rules::motionFile(t.file.c_str())) return false;
    text += t.file.substring(0, t.file.length()-4) + "," + String(t.weight, 4) + "\n";
  }
  return atomicText(cfg::WEIGHTS_PATH, text);
}
bool createPlaylist(const String& name, String& error) {
  if (!ready) { error = "SD-Karte fehlt."; return false; }
  if (!rules::playlistName(name.c_str())) { error = "Playlistname: 1-16 Zeichen, A-Z a-z 0-9 Leerzeichen _-().,; kein Rand-Leerzeichen/Punkt."; return false; }
  Guard lock;
  const String path = directory(name);
  if (SD.exists(path)) { error = "Dieser Ordner existiert bereits (Gross-/Kleinschreibung beachten)."; return false; }
  if (!SD.mkdir(path)) { error = "Ordner konnte nicht angelegt werden."; fault(error); return false; }
  Playlist p; p.name = name;
  if (!saveConfig(p)) { error = "Ordner angelegt, aber config.txt konnte nicht geschrieben werden."; return false; }
  return true;
}
bool checkMp3(const String& path, String& error) {
  Guard lock; File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) { error = "MP3 fehlt oder laesst sich nicht oeffnen."; healthy(); return false; }
  if (f.size() < 4 || f.size() > cfg::MAX_FILE_BYTES) { f.close(); error = "MP3 leer, zu kurz oder groesser als 256 MiB."; return false; }
  uint8_t h[3] = {}; const bool read = f.read(h, 3) == 3; f.close();
  if (!read) { error="MP3-Header nicht lesbar."; healthy(); return false; }
  if (!((h[0]=='I' && h[1]=='D' && h[2]=='3') || (h[0]==0xff && (h[1]&0xe0)==0xe0))) {
    error = "Kein ID3-/MP3-Frame-Header am Dateianfang. Datei neu als MP3 exportieren."; return false;
  }
  return true;
}
bool unicodeTest(String& report) {
  Guard lock;
  if (!ready) { report = "Keine SD-Karte."; return false; }
  // Testet exakt UTF-8 fuer ae-Umlaut, Eszett, Katakana und CJK sowie ,;.
  char suffix[12]; snprintf(suffix, sizeof(suffix), "%08lx", static_cast<unsigned long>(esp_random()));
  const String name = String("._utf8_") + "\xc3\xa4\xc3\x9f_\xe3\x82\xb7_\xe8\x89\xb2,;_" + suffix + ".tmp";
  const String path = String(cfg::PLAYLIST_DIR) + "/" + name;
  if (SD.exists(path)) { report = "Testpfad schon vorhanden; nicht veraendert."; return false; }
  File f = SD.open(path, FILE_WRITE);
  if (!f) { report = "UTF-8-Datei konnte nicht angelegt werden."; return false; }
  bool ok = f.print("utf8-ok") == 7; f.flush(); f.close();
  bool listed = false;
  File d = SD.open(cfg::PLAYLIST_DIR);
  if (d) {
    while (true) { String found = d.getNextFileName(); if (found.isEmpty()) break; if (found == path) listed = true; }
    d.close();
  }
  f = SD.open(path, FILE_READ);
  char bytes[8] = {};
  ok = ok && f && f.read(reinterpret_cast<uint8_t*>(bytes), 7) == 7 && String(bytes) == "utf8-ok";
  if (f) f.close();
  bool removed = SD.remove(path);
  ok = ok && listed && removed;
  report = ok ? "UTF-8-Test OK: Anlegen, bytegetreues Auflisten, Oeffnen, Lesen und Loeschen." :
                "UTF-8-Test fehlgeschlagen. Seriellen Monitor, Dateisystem und SDK-Konfiguration pruefen.";
  if (!removed) report += " Testdatei blieb auf SD: " + path;
  return ok;
}
}

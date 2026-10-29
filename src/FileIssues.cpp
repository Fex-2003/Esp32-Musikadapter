#include "FileIssues.h"
#include "Storage.h"
#include <esp_system.h>
namespace issues {
static std::vector<Entry> entries;
static bool dirty=false, loaded=false;
static uint32_t changed=0, boot=0;
static const char* path="/.audio-errors.tsv";
static constexpr size_t limit=64;
static bool safePath(const String& p) {
  return p.length()<=224 && (p.startsWith("/Playlists/") || p.startsWith("/Bewegungsmelder/")) &&
         p.indexOf("/../")<0 && p.indexOf('\t')<0 && p.indexOf('\n')<0 && p.indexOf('\r')<0;
}
void begin() {
  if(loaded || !storage::ready) return;
  loaded=true; boot=esp_random(); storage::Guard lock;
  // atomicText verwendet denselben Temp/Backup-Vertrag wie Playlist-Konfigurationen.
  storage::recoverText(path);
  File f=SD.open(path,FILE_READ);
  if(!f) return;
  while(f.available() && entries.size()<limit) {
    char line[420]; size_t n=f.readBytesUntil('\n',line,sizeof(line)-1); line[n]=0;
    String text(line); int a=text.indexOf('\t'),b=text.indexOf('\t',a+1),c=text.indexOf('\t',b+1),d=text.indexOf('\t',c+1);
    int e=text.indexOf('\t',d+1);
    if(a<0||b<0||c<0||d<0||e<0) continue;
    Entry item; item.path=text.substring(e+1); if(!safePath(item.path)) continue;
    item.boot=strtoul(text.c_str(),nullptr,10); item.seconds=text.substring(a+1,b).toInt();
    item.attempts=text.substring(b+1,c).toInt(); item.reason=text.substring(c+1,d);
    entries.push_back(item);
  }
  f.close();
}
void record(const String& p,const String& reason) {
  if(!safePath(p)) return;
  for(auto& e:entries) if(e.path==p) { e.reason=reason.substring(0,150); ++e.attempts; e.boot=boot; e.seconds=millis()/1000; dirty=true; changed=millis(); return; }
  if(entries.size()>=limit) return;
  Entry e; e.path=p; e.reason=reason.substring(0,150); e.boot=boot; e.seconds=millis()/1000; e.attempts=1;
  entries.push_back(e); dirty=true; changed=millis();
}
bool blocked(const String& p) {
  for(const auto& e:entries) if(e.path==p) return true;
  // Bei voller Liste konservativ stoppen, statt ungespeicherte Fehler endlos zu wiederholen.
  return entries.size()>=limit;
}
const std::vector<Entry>& list() { return entries; }
void clear(const String& p) {
  if(p.isEmpty()) entries.clear();
  else for(auto i=entries.begin();i!=entries.end();++i) if(i->path==p) { entries.erase(i); break; }
  dirty=true; changed=millis();
}
void flush(bool force) {
  if(!dirty || !storage::ready || (!force && uint32_t(millis()-changed)<5000)) return;
  size_t needed=0; for(const auto& e:entries) needed+=e.path.length()+e.reason.length()+38;
  String text;
  if(!text.reserve(needed+1)) { changed=millis(); Serial.println("[Fehlerliste] Speichern mangels RAM zurueckgestellt."); return; }
  for(const auto& e:entries) text+=String(e.boot)+"\t"+String(e.seconds)+"\t"+String(e.attempts)+"\t"+e.reason+"\t1\t"+e.path+"\n";
  if(storage::atomicText(path,text)) dirty=false;
}
bool retry(const String& p,String& message) {
  bool found=false; for(const auto& e:entries) if(e.path==p) found=true;
  if(!found) { message="Kein solcher Fehlerlisteneintrag."; return false; }
  if(!storage::checkMp3(p,message)) { if(storage::ready) record(p,message); return false; }
  clear(p); message="Header lesbar; fuer erneuten Wiedergabeversuch freigegeben. Vollstaendige Decoderpruefung erfolgt beim Abspielen."; return true;
}
}

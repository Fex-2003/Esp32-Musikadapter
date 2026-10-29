#include "BtDevices.h"
#include <Preferences.h>
#include <ctype.h>
namespace btdevices {
struct Store { uint32_t version=1; uint8_t used=0; bool eraseAll=false; uint8_t last[6]={}; Device devices[MAX_DEVICES]; };
static Store data;
static Preferences prefs;
static bool initialized=false,dirty=false;
bool equal(const uint8_t* a,const uint8_t* b) { return memcmp(a,b,6)==0; }
static bool nonzero(const uint8_t* a) { const uint8_t zero[6]={}; return !equal(a,zero); }
void begin() {
  if(initialized) return; initialized=true;
  if(!prefs.begin("audio-bt",false)) return;
  if(prefs.getBytesLength("devices")==sizeof(data)) {
    prefs.getBytes("devices",&data,sizeof(data));
    if(data.version!=1 || data.used>MAX_DEVICES) data=Store();
    for(auto& d:data.devices) d.name[64]=0;
  }
}
void save() { if(dirty && prefs.putBytes("devices",&data,sizeof(data))==sizeof(data)) dirty=false; }
size_t count() { return data.used; }
const Device& at(size_t i) { return data.devices[i]; }
String format(const uint8_t* mac) { char s[18]; snprintf(s,sizeof(s),"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]); return s; }
bool parse(const String& s,uint8_t* out) {
  if(s.length()!=17) return false;
  for(int i=0;i<6;++i) {
    if(i && s[i*3-1]!=':') return false;
    if(!isxdigit(static_cast<unsigned char>(s[i*3])) || !isxdigit(static_cast<unsigned char>(s[i*3+1]))) return false;
    char part[3]={s[i*3],s[i*3+1],0}; char* end=nullptr;
    unsigned long n=strtoul(part,&end,16); if(end!=part+2) return false; out[i]=n;
  } return nonzero(out);
}
bool preferred(uint8_t* mac) { if(!nonzero(data.last) || pending(data.last)) return false; memcpy(mac,data.last,6); return true; }
void confirm(const uint8_t* mac) {
  if(nonzero(mac) && !pending(mac) && !equal(data.last,mac)) { memcpy(data.last,mac,6); dirty=true; save(); }
}
bool remember(const uint8_t* mac,const char* name) {
  for(size_t i=0;i<data.used;++i) if(equal(data.devices[i].mac,mac)) {
    if(name && *name && strncmp(data.devices[i].name,name,64)!=0) { snprintf(data.devices[i].name,65,"%.64s",name); dirty=true; }
    return true;
  }
  if(data.used==MAX_DEVICES) return false;
  Device& d=data.devices[data.used++]; d=Device(); memcpy(d.mac,mac,6);
  if(name) snprintf(d.name,65,"%.64s",name); dirty=true; return true;
}
bool pending(const uint8_t* mac) {
  if(data.eraseAll) return true;
  for(size_t i=0;i<data.used;++i) if(equal(data.devices[i].mac,mac)) return data.devices[i].pending;
  return false;
}
bool pendingAll() { return data.eraseAll; }
bool remove(const String& s,String& message) {
  uint8_t mac[6]; if(!parse(s,mac)) { message="Ungueltige MAC-Adresse."; return false; }
  for(size_t i=0;i<data.used;++i) if(equal(data.devices[i].mac,mac)) {
    data.devices[i].pending=true; if(equal(data.last,mac)) memset(data.last,0,6);
    dirty=true; save();
    if(dirty) { message="Loeschauftrag konnte nicht in NVS gespeichert werden."; return false; }
    message="Wird beim naechsten Bluetooth-Start ausgefuehrt."; return true;
  }
  message="Geraet nicht mehr in der gespeicherten Liste."; return false;
}
bool removeAll(String& message) {
  data.eraseAll=true; memset(data.last,0,6); dirty=true; save();
  message=dirty?"NVS-Schreibfehler.":"Alle Kopplungen werden beim naechsten Bluetooth-Start entfernt."; return !dirty;
}
void reconcile(const uint8_t (*bonds)[6],size_t n) {
  for(size_t i=0;i<data.used;) {
    bool exists=false; for(size_t j=0;j<n;++j) if(equal(data.devices[i].mac,bonds[j])) exists=true;
    if(!exists) {
      if(equal(data.devices[i].mac,data.last)) memset(data.last,0,6);
      for(size_t j=i+1;j<data.used;++j) data.devices[j-1]=data.devices[j];
      data.devices[--data.used]=Device(); dirty=true;
    } else ++i;
  }
  for(size_t i=0;i<n;++i) remember(bonds[i],nullptr);
  if(data.eraseAll && n==0) { data.eraseAll=false; dirty=true; }
  if(nonzero(data.last)) { bool exists=false; for(size_t i=0;i<n;++i) if(equal(data.last,bonds[i])) exists=true; if(!exists) { memset(data.last,0,6); dirty=true; } }
  save();
}
}

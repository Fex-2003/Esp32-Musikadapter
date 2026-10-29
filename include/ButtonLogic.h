#pragma once
#include <stdint.h>

// Portable Tastenlogik, Zeitdifferenzen funktionieren auch beim millis-Ueberlauf.
namespace buttons {
enum Event : uint16_t { None=0, Mode=1, Up=2, Down=4, Play=8, PlayLong=16,
                       ModeLong=32, Interaction=64, LockChanged=128 };
class Logic {
  struct Key {
    bool raw=false, down=false, armed=false, consumed=false;
    uint32_t changed=0, pressed=0, threshold=0;
  } keys[4];
  struct Combo { bool latched=false, fired=false; uint32_t since=0; } all, mode;
  bool allLock=false, modeLock=false;
  uint32_t debounce, modeHold, playHold, comboTime, otherPlayHold;
  bool combo(Combo& c, int a, int b, uint32_t now, bool enabled) {
    if (!c.latched && enabled && keys[a].down && keys[b].down && keys[a].armed && keys[b].armed) {
      c.latched=true; c.fired=false; c.since=now;
    }
    if (!c.latched) return false;
    keys[a].consumed=keys[b].consumed=true;
    if (!enabled) c.fired=true;
    if (!keys[a].down || !keys[b].down) c.fired=true;
    if (!keys[a].down && !keys[b].down) { c.latched=false; return false; }
    if (!c.fired && enabled && uint32_t(now-c.since)>=comboTime) { c.fired=true; return true; }
    return false;
  }
 public:
  Logic(uint32_t d=35, uint32_t m=2000, uint32_t p=1500, uint32_t c=1000, uint32_t other=3000)
    :debounce(d),modeHold(m),playHold(p),comboTime(c),otherPlayHold(other) {}
  void begin(uint8_t pressed, uint32_t now) {
    for(int i=0;i<4;++i) {
      keys[i]=Key(); keys[i].raw=keys[i].down=(pressed & (1<<i));
      keys[i].armed=!keys[i].down; keys[i].changed=now;
    }
    all=Combo(); mode=Combo(); allLock=modeLock=false;
  }
  bool fullyLocked() const { return allLock; }
  bool modeLocked() const { return modeLock; }
  bool anyDown() const { for(const auto& k:keys) if(k.raw || k.down) return true; return false; }
  uint16_t poll(uint8_t pressed, uint32_t now, bool playlistActive=true) {
    uint16_t events=0; uint8_t released=0;
    for(int i=0;i<4;++i) {
      auto& k=keys[i]; const bool value=pressed & (1<<i);
      if(value!=k.raw) { k.raw=value; k.changed=now; }
      if(k.raw!=k.down && uint32_t(now-k.changed)>=debounce) {
        k.down=k.raw;
        if(k.down) {
          events|=Interaction; k.pressed=now; k.consumed=false;
          k.threshold=i==0?modeHold:(playlistActive?playHold:otherPlayHold);
        }
        else released|=1<<i;
      }
    }
    if(combo(all,1,2,now,true)) {
      allLock=!allLock; modeLock=false; events|=LockChanged;
      for(auto& k:keys) k.consumed=true;
    }
    if(combo(mode,0,3,now,!allLock && !all.latched)) {
      modeLock=!modeLock; events|=LockChanged;
    }
    for(int i=0;i<4;++i) {
      auto& k=keys[i];
      if(allLock || (i==0 && modeLock)) k.consumed=true;
      const bool partnerArriving=(i==0 && keys[3].raw) || (i==3 && keys[0].raw);
      if(k.down && k.raw && k.armed && !k.consumed && !partnerArriving && (i==0 || i==3) &&
         uint32_t(now-k.pressed)>=k.threshold) {
        events|=i==0?ModeLong:PlayLong; k.consumed=true;
      }
      if(released & (1<<i)) {
        if(k.armed && !k.consumed) events|=1<<i;
        k.armed=true;
      }
    }
    return events;
  }
};
}

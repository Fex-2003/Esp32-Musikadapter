#include "ButtonLogic.h"
#include "AudioEnvelope.h"
#include "Config.h"
#include "BtTransport.h"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace buttons;
struct Rig {
  Logic logic{cfg::BUTTON_DEBOUNCE_MS,cfg::BUTTON_MODE_HOLD_MS,cfg::BUTTON_PLAY_HOLD_MS,cfg::BUTTON_COMBO_MS};
  uint32_t now=0; uint8_t mask=0;
  Rig() { logic.begin(0,now); }
  uint16_t set(uint8_t value) { mask=value; logic.poll(mask,++now); now+=35; return logic.poll(mask,now); }
  uint16_t wait(uint32_t ms) { now+=ms; return logic.poll(mask,now); }
};
int main() {
  static_assert(cfg::BUTTON_UP==17 && cfg::BUTTON_DOWN==16,"Pin mapping");
  { Rig r; assert(r.set(Mode)==Interaction); assert(r.wait(200)==0); assert(r.set(0)==Mode); }
  { Rig r; r.set(Mode); assert(r.wait(1999)==0); assert(r.wait(1)==ModeLong); assert(r.wait(5000)==0); assert(r.set(0)==0); }
  { Rig r; r.set(Play); assert(r.wait(1499)==0); assert(r.wait(1)==PlayLong); assert(r.set(0)==0); }
  { Rig r; r.set(Play); assert(r.wait(500)==0); assert(r.set(0)==Play); }
  { Logic keys; keys.begin(0,0); keys.poll(Play,1); keys.poll(Play,36);
    assert(keys.poll(0,1526)==0); assert(keys.poll(0,1536)==0);
    assert(keys.poll(0,1561)==Play); } // Loslassen knapp vor 1,5 s bleibt kurz
  { Logic keys; keys.begin(0,0); keys.poll(Mode,1); keys.poll(Mode,36);
    assert(keys.poll(0,2026)==0); assert(keys.poll(0,2036)==0);
    assert(keys.poll(0,2061)==Mode); }
  { Logic keys; keys.begin(0,0); keys.poll(Play,1,false); assert(keys.poll(Play,36,false)==Interaction);
    assert(keys.poll(Play,2000,false)==0); keys.poll(0,2001,false);
    assert(keys.poll(0,2036,false)==Play); }
  { Logic keys; keys.begin(0,0); keys.poll(Play,1,false); keys.poll(Play,36,false);
    assert(keys.poll(Play,3036,true)==PlayLong); } // Schwelle gilt ab Druecken
  { Rig r; r.logic.begin(Mode,0); r.mask=Mode; assert(r.wait(4000)==0); assert(r.set(0)==0); r.set(Mode); assert(r.set(0)==Mode); }
  { Rig r; r.set(Mode); r.wait(1500); r.set(Mode|Play); assert(!(r.wait(500)&ModeLong));
    assert(r.wait(500)==LockChanged); assert(r.logic.modeLocked()); assert(r.wait(4000)==0);
    assert(r.set(Play)==0); assert(r.set(0)==0); r.set(Mode); assert(r.set(0)==0);
    r.set(Play); assert(r.set(0)==Play);
    r.set(Mode|Play); assert(r.wait(1000)==LockChanged); assert(!r.logic.modeLocked()); r.set(0); }
  { Rig r; r.set(Mode|Play); r.wait(500); assert(r.set(Mode)==0); assert(r.wait(5000)==0);
    assert(r.set(0)==0); r.set(Mode); assert(r.set(0)==Mode); }
  { Rig r; r.set(Up|Down); assert(r.wait(1000)==LockChanged); assert(r.logic.fullyLocked());
    r.set(0); assert(r.set(Play)==Interaction); assert(r.set(0)==0);
    r.set(Mode|Play); assert(r.wait(5000)==0); assert(!r.logic.modeLocked()); r.set(0);
    r.set(Up|Down); assert(r.wait(1000)==LockChanged); assert(!r.logic.fullyLocked()); assert(!r.logic.modeLocked());
    assert(r.set(0)==0); r.set(Up); assert(r.set(0)==Up); }
  { Rig r; r.set(Mode|Play); r.wait(1000); r.set(0); assert(r.logic.modeLocked());
    r.set(Up|Down); r.wait(1000); assert(r.logic.fullyLocked() && !r.logic.modeLocked()); }
  { Rig r; r.set(Mode|Play); r.wait(400); r.set(Mode|Play|Up|Down); r.wait(1000);
    assert(r.logic.fullyLocked() && !r.logic.modeLocked()); r.set(Mode|Play); r.wait(3000);
    r.set(Mode|Play|Up|Down); r.wait(1000); assert(!r.logic.fullyLocked() && !r.logic.modeLocked());
    r.set(0); assert(!r.logic.modeLocked()); }
  { Rig r; r.now=UINT32_MAX-100; r.set(Play); assert(r.wait(1500)==PlayLong); assert(r.set(0)==0); }
  { Rig r; r.set(Mode); r.wait(1990); r.mask=Mode|Play;
    assert(r.logic.poll(r.mask,r.now+10)==0); // zweite Taste noch nicht entprellt
    r.now+=45; assert(r.logic.poll(r.mask,r.now)==Interaction);
    assert(r.wait(1000)==LockChanged); assert(r.logic.modeLocked()); }
  { Rig r; r.set(Play); r.wait(1490); r.mask=Mode|Play;
    assert(r.logic.poll(r.mask,r.now+10)==0);
    r.now+=45; assert(r.logic.poll(r.mask,r.now)==Interaction);
    assert(r.wait(1000)==LockChanged); assert(r.logic.modeLocked()); }
  { Rig r; r.mask=Up; r.logic.poll(r.mask,10); assert(r.logic.poll(0,20)==0); assert(r.logic.poll(0,70)==0); }
  { audio::Envelope f; f.to(0,100); assert(f.value(100)==1); assert(std::abs(f.value(350)-0.5f)<0.001f);
    f.to(1,350); assert(std::abs(f.value(350)-0.5f)<0.001f); assert(f.value(850)==1); assert(f.done(850));
    f.to(0,UINT32_MAX-200); assert(f.value(299)==0); f.reset(); assert(f.value(10)==1); }
  assert(audio::userGain(0)==0); assert(audio::userGain(100)==1); assert(audio::userGain(50)==0.25f);
  using bt_transport::Playback;
  assert(bt_transport::shouldPause(Playback::Playing,false));
  assert(!bt_transport::shouldPause(Playback::Paused,true));
  assert(bt_transport::shouldPause(Playback::Unknown,true));
  assert(!bt_transport::shouldPause(Playback::Unknown,false));
  std::cout<<"Button/combination/lock/wraparound/fade tests passed\n";
}

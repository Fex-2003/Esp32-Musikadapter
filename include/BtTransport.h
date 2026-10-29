#pragma once
#include <stdint.h>
namespace bt_transport {
enum class Playback : uint8_t { Unknown, Playing, Paused };
// AVRCP ist massgeblich; andernfalls A2DP-Streamzustand verwenden. Nicht die
// Signalstaerke: digitale Stille kann Teil laufender Musik sein.
inline bool shouldPause(Playback reported, bool streamStarted) {
  return reported==Playback::Playing ||
         (reported==Playback::Unknown && streamStarted);
}
}

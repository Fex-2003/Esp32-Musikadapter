#pragma once
#include <stdint.h>
namespace audio {
class Envelope {
  float from=1, target=1;
  uint32_t began=0, duration=500;
 public:
  explicit Envelope(uint32_t ms=500):duration(ms) {}
  float value(uint32_t now) const {
    const uint32_t elapsed=now-began;
    return elapsed>=duration ? target : from+(target-from)*(float(elapsed)/duration);
  }
  void reset(float v=1) { from=target=v; }
  void to(float v, uint32_t now) { from=value(now); target=v; began=now; }
  bool done(uint32_t now) const { return from==target || uint32_t(now-began)>=duration; }
};
inline float userGain(uint8_t percent) {
  const float v=float(percent>100?100:percent)/100.0f;
  return v*v;
}
}

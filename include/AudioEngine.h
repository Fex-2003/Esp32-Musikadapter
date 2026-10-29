#pragma once
#include <Arduino.h>
namespace audio {
enum class State : uint8_t { Idle, Running, Ended, MotionTimeout, FileError, StorageError };
bool begin();
bool play(const String& path, uint8_t volume, String& error);
// Oeffnet und prueft einen erwarteten Folgetitel waehrend der laufenden Wiedergabe.
// Ein Fehler hier beeinflusst die aktuelle Wiedergabe nicht; play() hat stets einen
// vollstaendigen, pruefenden Fallback.
bool prepare(const String& path, String& error);
void discardPrepared();
void stop();
void fadeStop();
bool stopping();
bool pauseRequested();
String errorMessage();
bool fileFailed(); // Startfehler wirklich dateibezogen, nicht RAM/I2S
void pause(bool enabled);
bool paused();
void volume(uint8_t percent);
State state();
bool emittedSamples();
void motionGate(bool enabled, uint32_t delaySeconds);
bool motionWanted();
}

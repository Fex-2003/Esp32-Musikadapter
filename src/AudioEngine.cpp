#include "AudioEngine.h"
#include "Storage.h"
#include "Config.h"
#include "AudioEnvelope.h"
#include <AudioFileSourceSD.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <freertos/task.h>
#include <atomic>
#include <new>

namespace audio {
static SemaphoreHandle_t engineMutex = nullptr;
static TaskHandle_t taskHandle = nullptr;
static AudioFileSourceSD* source = nullptr;
static AudioFileSourceBuffer* buffer = nullptr;
static AudioGeneratorMP3* decoder = nullptr;
static uint8_t* inputBuffer = nullptr;
static void* decoderWorkspace = nullptr;
static std::atomic<State> current{State::Idle};
static std::atomic<bool> gate{false}, wanted{false}, pausedPlayback{false};
static std::atomic<uint32_t> graceMs{0}, acceptedSamples{0};
static std::atomic<bool> requestedPause{false}, requestedStop{false};
static std::atomic<bool> startFileFailure{false};
static Envelope envelope(cfg::AUDIO_FADE_MS);
static float userVolume = userGain(cfg::DEFAULT_SD_VOLUME);
static String lastError;
static bool readFailed = false;
static bool decoderFailed = false;
static uint32_t bytesWithoutSamples=0;
static unsigned decoderErrors=0;
static uint32_t lastProgress = 0, lastCount = 0;
static void decoderStatus(void*,int code,const char*) {
  // BUFLEN und LOSTSYNC sind auch beim normalen Puffer-/Tag-Ende moeglich.
  if(code!=0x0001 && code!=0x0101 && ++decoderErrors>=8) decoderFailed=true;
}
class CheckedSource : public AudioFileSourceSD {
 public:
  uint32_t read(void* data, uint32_t len) override {
    if(decoder && current.load()==State::Running &&
       (decoderFailed || bytesWithoutSamples>65536 || uint32_t(millis()-lastProgress)>3000)) {
      decoderFailed=true; return 0; // beendet auch die interne libmad-Retryschleife
    }
    const uint32_t before=getPos(), size=getSize();
    const uint32_t got=AudioFileSourceSD::read(data,len);
    bytesWithoutSamples+=got;
    if (len && before<size && got==0) readFailed=true;
    return got;
  }
};


class Output : public AudioOutputI2S {
 public:
  Output() : AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S, 12, AudioOutputI2S::APLL_DISABLE) {}
  bool ConsumeSample(int16_t sample[2]) override {
    const bool ok = AudioOutputI2S::ConsumeSample(sample);
    if (ok) {
      const uint32_t count=acceptedSamples.fetch_add(1, std::memory_order_relaxed);
      bytesWithoutSamples=0;
      if((count & 1023)==0) lastProgress=millis();
    }
    return ok;
  }
};
static Output* output = nullptr;
class Lock {
 public: Lock() { xSemaphoreTake(engineMutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(engineMutex); }
};
// Caller haelt Audio-Lock und SD-Lock. Pointer bleiben nur hier veraenderlich.
static bool skipId3v2(AudioFileSourceSD& file, String& error) {
  uint8_t header[10];
  if (file.read(header, sizeof(header)) != sizeof(header)) {
    error = "MP3-ID3-Header nicht lesbar."; return false;
  }
  if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
    if (!file.seek(0, SEEK_SET)) { error = "MP3-Dateianfang nicht erreichbar."; return false; }
    return true;
  }
  if ((header[6] & 0x80) || (header[7] & 0x80) || (header[8] & 0x80) || (header[9] & 0x80)) {
    error = "Ungueltige ID3v2-Taggroesse."; return false;
  }
  uint32_t skip = 10UL + (static_cast<uint32_t>(header[6]) << 21) +
                  (static_cast<uint32_t>(header[7]) << 14) +
                  (static_cast<uint32_t>(header[8]) << 7) + header[9];
  // Nur ID3v2.4 definiert das optionale 10-Byte-Footer-Bit.
  if (header[3] == 4 && (header[5] & 0x10)) skip += 10;
  if (skip >= file.getSize() || !file.seek(static_cast<int32_t>(skip), SEEK_SET)) {
    error = "ID3v2-Tag reicht ueber MP3-Datei hinaus."; return false;
  }
  return true;
}
// Caller haelt Audio-Lock und SD-Lock. Pointer bleiben nur hier veraenderlich.
static void cleanup() {
  requestedStop=false; requestedPause=false; pausedPlayback=false;
  if (decoder) {
    if (decoder->isRunning()) decoder->stop();
    delete decoder; decoder = nullptr;
  }
  delete buffer; buffer = nullptr;
  delete source; source = nullptr;
  if (output) output->stop();
}
static void task(void*) {
  bool sawMotion = false, oldGate = false, motionPaused = false;
  uint32_t lastHigh = 0, motionPausedAt = 0;
  for (;;) {
    const bool useGate = gate.load();
    if (useGate != oldGate) { sawMotion = motionPaused = false; oldGate = useGate; }
    uint32_t now = millis();
    const bool high = digitalRead(cfg::PIR) == HIGH;
    if (useGate && high) { sawMotion = true; lastHigh = now; }
    const bool need = useGate && (high || (sawMotion && static_cast<uint32_t>(now-lastHigh) < graceMs.load()));
    wanted.store(need);
    {
      Lock audioLock;
      now=millis(); // andere Task kann waehrend Lock-Warten einen Fade/Resume gestartet haben
      if (!decoder || current.load() != State::Running) {
        motionPaused = false;
      } else {
        storage::Guard sdLock;
        if (useGate && !requestedStop.load()) {
          if (motionPaused && uint32_t(now-motionPausedAt)>=graceMs.load()) {
            cleanup(); current=State::MotionTimeout; motionPaused=false;
          } else if (need && requestedPause.load()) {
            requestedPause=false; pausedPlayback=false; motionPaused=false;
            envelope.to(1,now); lastProgress=now;
          } else if (!need && !requestedPause.load()) {
            requestedPause=true; envelope.to(0,now);
          }
        }
        if (decoder) {
          output->SetGain(userVolume*envelope.value(now));
          if (envelope.done(now)) {
            if (requestedStop.load()) { cleanup(); current=State::Idle; requestedStop=false; }
            else if (requestedPause.load() && !pausedPlayback.load()) {
              pausedPlayback=true;
              if (useGate) { motionPaused=true; motionPausedAt=now; }
            }
          }
        }
        if (decoder && current.load() == State::Running && !pausedPlayback.load() &&
            (!decoder->isRunning() || !decoder->loop())) {
          // Letzte DMA-Samples ausspielen, dann alle Datei-Handles schliessen.
          if (output && decoder->isRunning() && acceptedSamples.load() > 256) output->flush();
          const bool broken=readFailed || decoderFailed || !decoder->isRunning() || acceptedSamples.load()<=256;
          lastError=readFailed ? "SD-Lesefehler innerhalb der MP3." : "MP3-Decoderfehler / keine dekodierbaren Samples.";
          cleanup();
          if (readFailed && !storage::healthy()) current=State::StorageError;
          else current=broken ? State::FileError : State::Ended;
        }
        if (decoder && !pausedPlayback.load()) {
          const uint32_t count=acceptedSamples.load();
          if(count!=lastCount) { lastCount=count; lastProgress=now; }
          else if(uint32_t(now-lastProgress)>3000) {
            lastError="MP3-Decoder liefert seit 3 s keine Samples.";
            cleanup(); current=storage::healthy()?State::FileError:State::StorageError;
          }
        }
      }
    }
    // Audio laeuft unabhaengig von synchronen Browserantworten. Kein Busy-Wait.
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
bool begin() {
  engineMutex = xSemaphoreCreateMutex();
  if (!engineMutex) return false;
  output = new (std::nothrow) Output();
  if (!output) return false;
  output->SetMclk(false);
  output->SwapClocks(false);
  output->SetPinout(cfg::I2S_BCK, cfg::I2S_LCK, cfg::I2S_DOUT);
  return xTaskCreatePinnedToCore(task, "sd-audio", cfg::AUDIO_TASK_STACK, nullptr, 2, &taskHandle, 1) == pdPASS;
}
void stop() {
  if (!engineMutex) return;
  Lock audioLock; storage::Guard sdLock;
  pausedPlayback=false; requestedPause=false; requestedStop=false;
  envelope.reset(); cleanup(); current.store(State::Idle);
}
void pause(bool enabled) {
  if (!engineMutex) return;
  Lock lock;
  if (current.load()!=State::Running || requestedStop.load() || enabled==requestedPause.load()) return;
  requestedPause=enabled; envelope.to(enabled?0:1,millis());
  if (!enabled) { pausedPlayback=false; lastProgress=millis(); }
}
void fadeStop() {
  if (!engineMutex) return;
  Lock lock;
  if(current.load()!=State::Running || requestedStop.load()) return;
  requestedStop=true; envelope.to(0,millis());
  if(pausedPlayback.load()) envelope.reset(0);
}
bool stopping() { return requestedStop.load(); }
bool pauseRequested() { return requestedPause.load(); }
String errorMessage() { if(!engineMutex) return ""; Lock lock; return lastError; }
bool fileFailed() { return startFileFailure.load(); }
bool paused() { return pausedPlayback.load(); }
bool play(const String& path, uint8_t percent, String& error) {
  startFileFailure=false;
  if (!engineMutex || !taskHandle || !output || !storage::ready) { error = "Audio/SD nicht bereit."; return false; }
  Lock audioLock; storage::Guard sdLock;
  pausedPlayback=false; requestedPause=false; requestedStop=false; envelope.reset();
  readFailed=false; decoderFailed=false; decoderErrors=0; bytesWithoutSamples=0;
  lastError=""; lastCount=0; lastProgress=millis();
  cleanup(); current.store(State::Idle); acceptedSamples.store(0);
  if (!storage::checkMp3(path, error)) { startFileFailure=true; return false; }
  // Libmad-Arbeitsbereich einmal je SD-Boot reservieren, nicht pro Musikdatei.
  const size_t workBytes = AudioGeneratorMP3::preAllocSize();
  if (!decoderWorkspace) {
    if (ESP.getFreeHeap() < workBytes + cfg::MP3_BUFFER_BYTES + cfg::MIN_HEAP_RESERVE) {
      error = "Zu wenig RAM fuer MP3-Decoder und Sicherheitsreserve."; return false;
    }
    decoderWorkspace = malloc(workBytes);
  }
  if (!inputBuffer) inputBuffer = static_cast<uint8_t*>(malloc(cfg::MP3_BUFFER_BYTES));
  if (!decoderWorkspace || !inputBuffer) { error = "MP3-Puffer konnte nicht reserviert werden."; return false; }
  source = new (std::nothrow) CheckedSource();
  if(!source) { error="MP3-Objekt konnte nicht angelegt werden."; cleanup(); return false; }
  if(!source->open(path.c_str())) { error="MP3-Datei nicht geoeffnet."; startFileFailure=true; cleanup(); return false; }
  if (!skipId3v2(*source, error)) { startFileFailure=true; cleanup(); return false; }
  buffer = new (std::nothrow) AudioFileSourceBuffer(source, inputBuffer, cfg::MP3_BUFFER_BYTES);
  // Preallocation vermeidet die ungueltigen freien Pointer bestimmter 1.9.9-
  // Fehlerpfade vor begin(). Speicherbesitz bleibt explizit bei dieser Engine.
  decoder = new (std::nothrow) AudioGeneratorMP3(decoderWorkspace, static_cast<int>(workBytes));
  if (!buffer || !decoder) { error = "MP3-Objekt konnte nicht angelegt werden."; cleanup(); return false; }
  decoder->RegisterStatusCB(decoderStatus,nullptr);
  userVolume=userGain(percent); output->SetGain(userVolume);
  if (!decoder->begin(buffer, output)) { error = "MP3-Decoder/I2S konnte nicht starten."; cleanup(); return false; }
  bytesWithoutSamples=0; lastProgress=millis();
  current.store(State::Running);
  Serial.printf("[Audio] %s | Heap=%u | Decoder=%u B\n", path.c_str(), ESP.getFreeHeap(), static_cast<unsigned>(workBytes));
  return true;
}
void volume(uint8_t percent) {
  if (!engineMutex || !output) return;
  Lock lock;
  userVolume=userGain(percent); output->SetGain(userVolume*envelope.value(millis()));
}
State state() { return current.load(); }
bool emittedSamples() { return acceptedSamples.load() > 256; }
void motionGate(bool enabled, uint32_t seconds) {
  graceMs.store(seconds*1000UL); gate.store(enabled);
  if (!enabled) wanted.store(false);
}
bool motionWanted() { return wanted.load(); }
}

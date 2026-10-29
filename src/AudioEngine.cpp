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
#include <algorithm>

#if AUDIO_MP3_TRANSITION_TRACE
#define MP3_TRACE_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define MP3_TRACE_PRINTF(...) do {} while (0)
#endif

namespace audio {
class CheckedSource;
static SemaphoreHandle_t engineMutex = nullptr;
static TaskHandle_t taskHandle = nullptr;
static CheckedSource* source = nullptr;
static AudioFileSourceBuffer* buffer = nullptr;
static AudioGeneratorMP3* decoder = nullptr;
static uint8_t* inputBuffer = nullptr;
static void* decoderWorkspace = nullptr;
static std::atomic<State> current{State::Idle};
static std::atomic<bool> gate{false}, wanted{false}, pausedPlayback{false};
static std::atomic<uint32_t> graceMs{0}, acceptedSamples{0};
// TEMPORARY MP3 transition diagnostics; remove after timing measurements.
static std::atomic<uint32_t> playRequestedAt{0}, firstSampleAt{0}, firstDecoderLoopAt{0};
static std::atomic<bool> firstSamplePending{false}, firstDecoderIterationPending{false};
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
static void trace(const char* event) {
  MP3_TRACE_PRINTF("[MP3 t=%lu] AUDIO: %s\n", static_cast<unsigned long>(millis()), event);
}
static void traceDuration(const char* event, uint32_t startedAt) {
  MP3_TRACE_PRINTF("[MP3 t=%lu] AUDIO: %s (%lu ms)\n", static_cast<unsigned long>(millis()), event,
                   static_cast<unsigned long>(millis()-startedAt));
}
static void decoderStatus(void*,int code,const char*) {
  // BUFLEN und LOSTSYNC sind auch beim normalen Puffer-/Tag-Ende moeglich.
  if(code!=0x0001 && code!=0x0101 && ++decoderErrors>=8) decoderFailed=true;
}
class CheckedSource : public AudioFileSourceSD {
 public:
  ~CheckedSource() { if (prefetched) free(prefetched); }
  void monitor(bool enabled) { monitored=enabled; }
  uint32_t prefetchInitial() {
    if (prefetched || !getSize()) return 0;
    prefetched=static_cast<uint8_t*>(malloc(cfg::MP3_BUFFER_BYTES));
    if (!prefetched) return 0;
    prefetchedLength=AudioFileSourceSD::read(prefetched,cfg::MP3_BUFFER_BYTES);
    prefetchedOffset=0;
    if (!prefetchedLength) { free(prefetched); prefetched=nullptr; }
    return prefetchedLength;
  }
  uint32_t getPos() override {
    const uint32_t actual=AudioFileSourceSD::getPos();
    const uint32_t remaining=prefetchedLength-prefetchedOffset;
    return actual>=remaining ? actual-remaining : 0;
  }
  uint32_t read(void* data, uint32_t len) override {
    if(monitored && decoder && current.load()==State::Running &&
       (decoderFailed || bytesWithoutSamples>65536 || uint32_t(millis()-lastProgress)>3000)) {
      decoderFailed=true; return 0; // beendet auch die interne libmad-Retryschleife
    }
    const uint32_t before=getPos(), size=getSize();
    uint32_t got=0;
    if (prefetchedOffset<prefetchedLength) {
      const uint32_t cached=std::min(len,prefetchedLength-prefetchedOffset);
      memcpy(data,prefetched+prefetchedOffset,cached);
      prefetchedOffset+=cached; got+=cached;
      if(prefetchedOffset==prefetchedLength) {
        free(prefetched); prefetched=nullptr; prefetchedLength=prefetchedOffset=0;
      }
    }
    if (got<len) got+=AudioFileSourceSD::read(static_cast<uint8_t*>(data)+got,len-got);
    if(monitored) {
      bytesWithoutSamples+=got;
      if (len && before<size && got==0) readFailed=true;
    }
    return got;
  }
 private:
  uint8_t* prefetched=nullptr;
  uint32_t prefetchedLength=0, prefetchedOffset=0;
  bool monitored=true;
};


class Output : public AudioOutputI2S {
 public:
  Output() : AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S, 12, AudioOutputI2S::APLL_DISABLE) {}
  void retainI2s(bool retain) { retained=retain; }
  bool retainsI2s() const { return retained; }
  bool begin() override {
    // Beim fehlerfreien Titeluebergang laufen die letzten DMA-Samples weiter.
    if(retained) return true;
    return AudioOutputI2S::begin();
  }
  bool stop() override {
    if(retained) return true;
    return AudioOutputI2S::stop();
  }
  bool ConsumeSample(int16_t sample[2]) override {
    const bool ok = AudioOutputI2S::ConsumeSample(sample);
    if (ok) {
      const uint32_t count=acceptedSamples.fetch_add(1, std::memory_order_relaxed);
      if (count == 0) {
        firstSampleAt.store(millis(), std::memory_order_relaxed);
        firstSamplePending.store(true, std::memory_order_release);
      }
      bytesWithoutSamples=0;
      if((count & 1023)==0) lastProgress=millis();
    }
    return ok;
  }
 private:
  bool retained=false;
};
static Output* output = nullptr;
static CheckedSource* preparedSource = nullptr;
static String preparedPath;
class Lock {
 public: Lock() { xSemaphoreTake(engineMutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(engineMutex); }
};
// Caller haelt Audio-Lock und SD-Lock. Pointer bleiben nur hier veraenderlich.
static void clearPrepared() {
  delete preparedSource; preparedSource=nullptr; preparedPath="";
}
static bool validateAndSkipId3v2(AudioFileSourceSD& file, String& error) {
  if(file.getSize()<4 || file.getSize()>cfg::MAX_FILE_BYTES) {
    error="MP3 leer, zu kurz oder groesser als 256 MiB."; return false;
  }
  uint8_t header[10];
  if (file.read(header, sizeof(header)) != sizeof(header)) {
    error = "MP3-ID3-Header nicht lesbar."; return false;
  }
  if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
    if(header[0]!=0xff || (header[1]&0xe0)!=0xe0) {
      error="Kein ID3-/MP3-Frame-Header am Dateianfang."; return false;
    }
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
static void cleanup(bool retainI2s=false) {
  const uint32_t cleanupStartedAt=millis();
  if(output) output->retainI2s(retainI2s);
  requestedStop=false; requestedPause=false; pausedPlayback=false;
  if (decoder) {
    if (decoder->isRunning()) decoder->stop();
    delete decoder; decoder = nullptr;
  }
  delete buffer; buffer = nullptr;
  delete source; source = nullptr;
  if (output) output->stop();
  traceDuration(retainI2s ? "cleanup (Decoder/Buffer/SD; I2S retained)" : "cleanup (Decoder/Buffer/SD/I2S)", cleanupStartedAt);
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
        const bool firstIteration=firstDecoderIterationPending.exchange(false, std::memory_order_acq_rel);
        const uint32_t iterationStartedAt=millis();
        if (firstIteration) firstDecoderLoopAt.store(iterationStartedAt, std::memory_order_relaxed);
        if (decoder && current.load() == State::Running && !pausedPlayback.load() &&
            (!decoder->isRunning() || !decoder->loop())) {
          trace("decoder end/error detected in Audio-Task");
          const bool broken=readFailed || decoderFailed || !decoder->isRunning() || acceptedSamples.load()<=256;
          lastError=readFailed ? "SD-Lesefehler innerhalb der MP3." : "MP3-Decoderfehler / keine dekodierbaren Samples.";
          if(!broken) trace("normal end: I2S DMA tail retained for handoff");
          cleanup(!broken);
          if (readFailed && !storage::healthy()) current=State::StorageError;
          else current=broken ? State::FileError : State::Ended;
          trace("terminal audio state published");
        }
        if (firstIteration) traceDuration("first decoder iteration returned", iterationStartedAt);
        if (firstSamplePending.exchange(false, std::memory_order_acq_rel)) {
          const uint32_t sampleAt=firstSampleAt.load(std::memory_order_relaxed);
          const uint32_t requestedAt=playRequestedAt.load(std::memory_order_relaxed);
          const uint32_t loopAt=firstDecoderLoopAt.load(std::memory_order_relaxed);
          MP3_TRACE_PRINTF("[MP3 t=%lu] AUDIO: first I2S sample accepted; since play request=%lu ms, since decoder iteration=%lu ms\n",
                           static_cast<unsigned long>(sampleAt),
                           static_cast<unsigned long>(sampleAt-requestedAt),
                           static_cast<unsigned long>(sampleAt-loopAt));
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
  const uint32_t stopStartedAt=millis();
  trace("audio::stop begin");
  Lock audioLock; storage::Guard sdLock;
  traceDuration("audio::stop lock acquisition", stopStartedAt);
  pausedPlayback=false; requestedPause=false; requestedStop=false;
  envelope.reset(); cleanup(); current.store(State::Idle);
  trace("audio::stop end; state=Idle");
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
  const uint32_t playStartedAt=millis();
  playRequestedAt.store(playStartedAt, std::memory_order_relaxed);
  firstSamplePending.store(false, std::memory_order_relaxed);
  firstDecoderIterationPending.store(false, std::memory_order_relaxed);
  trace("audio::play begin");
  startFileFailure=false;
  if (!engineMutex || !taskHandle || !output || !storage::ready) { error = "Audio/SD nicht bereit."; return false; }
  const uint32_t audioLockStartedAt=millis();
  Lock audioLock;
  traceDuration("audio::play audio lock acquisition", audioLockStartedAt);
  const uint32_t sdLockStartedAt=millis();
  storage::Guard sdLock;
  traceDuration("audio::play SD lock acquisition", sdLockStartedAt);
  pausedPlayback=false; requestedPause=false; requestedStop=false; envelope.reset();
  readFailed=false; decoderFailed=false; decoderErrors=0; bytesWithoutSamples=0;
  lastError=""; lastCount=0; lastProgress=millis();
  const bool handoff=output->retainsI2s();
  cleanup(handoff); current.store(State::Idle); acceptedSamples.store(0);
  uint32_t phaseStartedAt=millis();
  const bool usePrepared=preparedSource && preparedPath==path;
  if(usePrepared) {
    source=preparedSource; preparedSource=nullptr; preparedPath="";
    source->monitor(true);
    traceDuration("prepared source adopted", phaseStartedAt);
  } else {
    clearPrepared();
    source = new (std::nothrow) CheckedSource();
    if(!source) { traceDuration("SD source allocation failed", phaseStartedAt); error="MP3-Objekt konnte nicht angelegt werden."; cleanup(); return false; }
    source->monitor(false);
    if(!source->open(path.c_str())) { traceDuration("SD file open failed", phaseStartedAt); error="MP3-Datei nicht geoeffnet."; startFileFailure=true; cleanup(); return false; }
    traceDuration("SD file open", phaseStartedAt);
    phaseStartedAt=millis();
    if (!validateAndSkipId3v2(*source, error)) { traceDuration("MP3 header/ID3 validation failed", phaseStartedAt); startFileFailure=true; cleanup(); return false; }
    traceDuration("MP3 header/ID3 validation", phaseStartedAt);
    source->monitor(true);
  }
  // Libmad-Arbeitsbereich einmal je SD-Boot reservieren, nicht pro Musikdatei.
  phaseStartedAt=millis();
  const size_t workBytes = AudioGeneratorMP3::preAllocSize();
  if (!decoderWorkspace) {
    if (ESP.getFreeHeap() < workBytes + cfg::MP3_BUFFER_BYTES + cfg::MIN_HEAP_RESERVE) {
      traceDuration("decoder RAM reserve check failed", phaseStartedAt);
      error = "Zu wenig RAM fuer MP3-Decoder und Sicherheitsreserve."; return false;
    }
    decoderWorkspace = malloc(workBytes);
  }
  if (!inputBuffer) inputBuffer = static_cast<uint8_t*>(malloc(cfg::MP3_BUFFER_BYTES));
  if (!decoderWorkspace || !inputBuffer) { traceDuration("decoder/input buffer allocation failed", phaseStartedAt); error = "MP3-Puffer konnte nicht reserviert werden."; return false; }
  traceDuration("decoder workspace/input buffer preparation", phaseStartedAt);
  phaseStartedAt=millis();
  buffer = new (std::nothrow) AudioFileSourceBuffer(source, inputBuffer, cfg::MP3_BUFFER_BYTES);
  // Preallocation vermeidet die ungueltigen freien Pointer bestimmter 1.9.9-
  // Fehlerpfade vor begin(). Speicherbesitz bleibt explizit bei dieser Engine.
  decoder = new (std::nothrow) AudioGeneratorMP3(decoderWorkspace, static_cast<int>(workBytes));
  if (!buffer || !decoder) { traceDuration("decoder/buffer object allocation failed", phaseStartedAt); error = "MP3-Objekt konnte nicht angelegt werden."; cleanup(); return false; }
  decoder->RegisterStatusCB(decoderStatus,nullptr);
  userVolume=userGain(percent); output->SetGain(userVolume);
  traceDuration("source/buffer/decoder object preparation", phaseStartedAt);
  phaseStartedAt=millis();
  if (!decoder->begin(buffer, output)) { traceDuration("decoder->begin failed", phaseStartedAt); error = "MP3-Decoder/I2S konnte nicht starten."; cleanup(); return false; }
  traceDuration("decoder->begin", phaseStartedAt);
  output->retainI2s(false);
  bytesWithoutSamples=0; lastProgress=millis();
  firstDecoderIterationPending.store(true, std::memory_order_release);
  current.store(State::Running);
  MP3_TRACE_PRINTF("[MP3 t=%lu] AUDIO: decoder running; total audio::play=%lu ms | %s | Heap=%u | Decoder=%u B\n",
                   static_cast<unsigned long>(millis()), static_cast<unsigned long>(millis()-playStartedAt),
                   path.c_str(), ESP.getFreeHeap(), static_cast<unsigned>(workBytes));
  return true;
}
bool prepare(const String& path, String& error) {
  error="";
  if(!engineMutex || !storage::ready || path.isEmpty()) { error="Audio/SD nicht bereit."; return false; }
  if(ESP.getFreeHeap()<cfg::MIN_HEAP_RESERVE+cfg::MP3_BUFFER_BYTES) {
    error="MP3-Vorbereitung: Heap-Sicherheitsreserve."; return false;
  }
  {
    Lock audioLock;
    if(preparedSource && preparedPath==path) return true;
  }
  const uint32_t startedAt=millis();
  CheckedSource* candidate=nullptr;
  {
    storage::Guard sdLock;
    candidate=new (std::nothrow) CheckedSource();
    if(!candidate) { error="MP3-Vorbereitung: Speicher fehlt."; return false; }
    candidate->monitor(false);
    if(!candidate->open(path.c_str())) {
      error="MP3-Vorbereitung: Datei nicht geoeffnet."; delete candidate; return false;
    }
    if(!validateAndSkipId3v2(*candidate,error)) {
      error="MP3-Vorbereitung: "+error; delete candidate; return false;
    }
    const uint32_t cached=candidate->prefetchInitial();
    MP3_TRACE_PRINTF("[MP3 t=%lu] AUDIO: prepared next track (%lu cached bytes, %lu ms): %s\n",
                     static_cast<unsigned long>(millis()), static_cast<unsigned long>(cached),
                     static_cast<unsigned long>(millis()-startedAt), path.c_str());
  }
  {
    Lock audioLock; storage::Guard sdLock;
    clearPrepared();
    preparedSource=candidate; preparedPath=path;
  }
  return true;
}
void discardPrepared() {
  if(!engineMutex) return;
  Lock audioLock; storage::Guard sdLock;
  clearPrepared();
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

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
// Xing/Info mit LAME-kompatibler Erweiterung beschreibt den nicht zur Musik
// gehoerenden Encoder-/Decoder-Vorlauf sowie das Padding am Dateiende.
// rawEnd ist der exklusive Index der letzten auszugebenden MP3-Samples.
struct GaplessInfo {
  uint32_t leadingSkip=0;
  uint32_t rawEnd=0;
  bool valid=false;
};
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
  void beginTrack(const GaplessInfo& info, uint32_t rawPosition=0, bool decoderPrimer=true) {
    gapless=info; rawIndex=rawPosition; primerPending=decoderPrimer;
  }
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
    // AudioGeneratorMP3 liefert vor dem ersten dekodierten Sample ein
    // konstruktionsbedingtes Nullsample. Es gehoert nicht zum MP3-Stream.
    if(primerPending) { primerPending=false; return true; }
    if(gapless.valid && rawIndex<gapless.leadingSkip) { ++rawIndex; return true; }
    if(gapless.valid && rawIndex>=gapless.rawEnd) { ++rawIndex; return true; }
    const bool ok = AudioOutputI2S::ConsumeSample(sample);
    if (ok) {
      ++rawIndex;
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
  bool primerPending=true;
  uint32_t rawIndex=0;
  GaplessInfo gapless;
};
class PcmPrefetchOutput : public AudioOutput {
 public:
  void reset(int16_t* target, const GaplessInfo& info=GaplessInfo()) {
    samples=target; produced=0; quota=0; written=0; sampleRate=0;
    rawIndex=rawConsumed=firstRawIndex=0; firstStored=false; primerPending=true; gapless=info;
  }
  void beginSlice(uint32_t frames) { quota=frames; written=0; }
  bool begin() override { return samples!=nullptr; }
  bool SetRate(int hz) override { sampleRate=hz; return AudioOutput::SetRate(hz); }
  bool stop() override { return true; }
  bool ConsumeSample(int16_t sample[2]) override {
    if(primerPending) { primerPending=false; return true; }
    if(gapless.valid && rawIndex<gapless.leadingSkip) { ++rawIndex; ++rawConsumed; return true; }
    if(gapless.valid && rawIndex>=gapless.rawEnd) { ++rawIndex; ++rawConsumed; return true; }
    if(!samples || produced>=cfg::PCM_PREFETCH_FRAMES || written>=quota) return false;
    if(!firstStored) { firstRawIndex=rawIndex; firstStored=true; }
    samples[produced*2]=sample[0]; samples[produced*2+1]=sample[1];
    ++produced; ++written; ++rawIndex; ++rawConsumed;
    return true;
  }
  uint32_t frames() const { return produced; }
  bool full() const { return produced>=cfg::PCM_PREFETCH_FRAMES; }
  int rate() const { return sampleRate; }
  uint32_t rawFrames() const { return rawConsumed; }
  uint32_t firstRawFrame() const { return firstRawIndex; }
 private:
  int16_t* samples=nullptr;
  uint32_t produced=0, quota=0, written=0, rawIndex=0, rawConsumed=0, firstRawIndex=0;
  int sampleRate=0;
  bool firstStored=false, primerPending=true;
  GaplessInfo gapless;
};
class CatchupOutput : public AudioOutput {
 public:
  void reset(uint32_t frames) { remaining=frames; caught=false; primerPending=true; }
  bool begin() override { return true; }
  bool stop() override { return true; }
  bool ConsumeSample(int16_t[]) override {
    if(primerPending) { primerPending=false; return true; }
    if(!remaining) { caught=true; return false; }
    --remaining;
    return true;
  }
  bool caughtUp() const { return caught; }
 private:
  uint32_t remaining=0;
  bool caught=false;
  bool primerPending=true;
};
class HandoffDecoder : public AudioGeneratorMP3 {
 public:
  HandoffDecoder(void* workspace, int bytes) : AudioGeneratorMP3(workspace,bytes) {}
  void switchOutput(AudioOutput* target) {
    output=target;
    output->SetBitsPerSample(16); output->SetChannels(2);
    lastRate=0; lastChannels=0;
  }
};
static Output* output = nullptr;
static CheckedSource* preparedSource = nullptr;
static String preparedPath;
static GaplessInfo preparedGapless;
static GaplessInfo currentGapless;
static CheckedSource* pcmSource = nullptr;
static AudioFileSourceBuffer* pcmBuffer = nullptr;
static AudioGeneratorMP3* pcmDecoder = nullptr;
static uint8_t* pcmInputBuffer = nullptr;
static void* pcmWorkspace = nullptr;
static int16_t* pcmSamples = nullptr;
static String pcmPath;
static GaplessInfo pcmGapless;
static bool pcmBuilding=false, pcmReady=false, pcmHandoff=false, pcmCaughtUp=false, pcmCatchupEnded=false;
static uint32_t pcmRead=0;
static PcmPrefetchOutput pcmOutput;
static CatchupOutput catchupOutput;
static HandoffDecoder* handoffDecoder = nullptr;
class Lock {
 public: Lock() { xSemaphoreTake(engineMutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(engineMutex); }
};
// Caller haelt Audio-Lock und SD-Lock. Pointer bleiben nur hier veraenderlich.
static void clearPrepared() {
  delete preparedSource; preparedSource=nullptr; preparedPath=""; preparedGapless=GaplessInfo();
}
static uint32_t be32(const uint8_t* value) {
  return (static_cast<uint32_t>(value[0])<<24) | (static_cast<uint32_t>(value[1])<<16) |
         (static_cast<uint32_t>(value[2])<<8) | value[3];
}
// Liest nur den ersten MP3-Frame. Bei unbekannten/zweifelhaften Angaben bleibt
// valid=false und die Wiedergabe verwendet unveraendert den sicheren Standardpfad.
static void inspectGaplessInfo(AudioFileSourceSD& file, uint32_t audioStart, GaplessInfo& result) {
  result=GaplessInfo();
  uint8_t header[4];
  if(!file.seek(static_cast<int32_t>(audioStart),SEEK_SET) || file.read(header,sizeof(header))!=sizeof(header)) return;
  const uint8_t version=(header[1]>>3)&0x03;
  const uint8_t layer=(header[1]>>1)&0x03;
  const uint8_t rateIndex=(header[2]>>2)&0x03;
  if(header[0]!=0xff || (header[1]&0xe0)!=0xe0 || version==1 || layer!=1 || rateIndex==3) {
    file.seek(static_cast<int32_t>(audioStart),SEEK_SET); return;
  }
  const bool mpeg1=version==3;
  const bool crcPresent=(header[1]&0x01)==0;
  const bool mono=((header[3]>>6)&0x03)==3;
  const uint32_t sideInfo=mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
  const uint32_t infoOffset=audioStart+4+(crcPresent ? 2 : 0)+sideInfo;
  uint8_t marker[4], flagsBytes[4];
  if(!file.seek(static_cast<int32_t>(infoOffset),SEEK_SET) ||
     file.read(marker,sizeof(marker))!=sizeof(marker) || file.read(flagsBytes,sizeof(flagsBytes))!=sizeof(flagsBytes)) {
    file.seek(static_cast<int32_t>(audioStart),SEEK_SET); return;
  }
  if((memcmp(marker,"Xing",4)!=0 && memcmp(marker,"Info",4)!=0) || !(be32(flagsBytes)&0x01)) {
    file.seek(static_cast<int32_t>(audioStart),SEEK_SET); return;
  }
  uint8_t framesBytes[4];
  if(file.read(framesBytes,sizeof(framesBytes))!=sizeof(framesBytes)) {
    file.seek(static_cast<int32_t>(audioStart),SEEK_SET); return;
  }
  const uint32_t frameCount=be32(framesBytes);
  uint32_t extensionOffset=infoOffset+12;
  const uint32_t flags=be32(flagsBytes);
  if(flags&0x02) extensionOffset+=4;
  if(flags&0x04) extensionOffset+=100;
  if(flags&0x08) extensionOffset+=4;
  uint8_t extension[24];
  if(!frameCount || !file.seek(static_cast<int32_t>(extensionOffset),SEEK_SET) ||
     file.read(extension,sizeof(extension))!=sizeof(extension)) {
    file.seek(static_cast<int32_t>(audioStart),SEEK_SET); return;
  }
  // LAME und FFmpeg/Lavc schreiben dieselbe dreibyte Delay/Padding-Struktur.
  const bool lameStyle=(memcmp(extension,"LAME",4)==0 || memcmp(extension,"Lav",3)==0);
  const uint32_t encoderDelay=(static_cast<uint32_t>(extension[21])<<4) | (extension[22]>>4);
  const uint32_t encoderPadding=(static_cast<uint32_t>(extension[22]&0x0f)<<8) | extension[23];
  constexpr uint32_t LIBMAD_SYNTH_DELAY=529;
  const uint32_t leadingSkip=encoderDelay+LIBMAD_SYNTH_DELAY;
  const uint32_t trailingDiscard=encoderPadding>LIBMAD_SYNTH_DELAY ? encoderPadding-LIBMAD_SYNTH_DELAY : 0;
  const uint32_t samplesPerFrame=mpeg1 ? 1152 : 576;
  const uint64_t totalSamples=static_cast<uint64_t>(frameCount)*samplesPerFrame;
  if(lameStyle && totalSamples>leadingSkip+trailingDiscard && totalSamples<=UINT32_MAX) {
    result.leadingSkip=leadingSkip;
    result.rawEnd=static_cast<uint32_t>(totalSamples-trailingDiscard);
    result.valid=true;
    MP3_TRACE_PRINTF("[MP3 t=%lu] AUDIO: gapless trim: skip=%lu, discard=%lu, frames=%lu\n",
                     static_cast<unsigned long>(millis()), static_cast<unsigned long>(leadingSkip),
                     static_cast<unsigned long>(trailingDiscard), static_cast<unsigned long>(frameCount));
  }
  file.seek(static_cast<int32_t>(audioStart),SEEK_SET);
}
// Der Xing/Info-Frame traegt nur VBR-/Gapless-Metadaten. Libmad wertet ihn
// nicht selbst aus; daher beginnt die eigentliche MP3-Quelle danach.
static void skipXingInfoFrame(AudioFileSourceSD& file, uint32_t audioStart) {
  uint8_t header[4], marker[4];
  if(!file.seek(static_cast<int32_t>(audioStart),SEEK_SET) || file.read(header,sizeof(header))!=sizeof(header)) return;
  const uint8_t version=(header[1]>>3)&0x03;
  const uint8_t layer=(header[1]>>1)&0x03;
  const uint8_t rateIndex=(header[2]>>2)&0x03;
  const uint8_t bitrateIndex=(header[2]>>4)&0x0f;
  if(header[0]!=0xff || (header[1]&0xe0)!=0xe0 || version==1 || layer!=1 || rateIndex==3 || !bitrateIndex || bitrateIndex==15) {
    file.seek(static_cast<int32_t>(audioStart),SEEK_SET); return;
  }
  const bool mpeg1=version==3;
  const bool crcPresent=(header[1]&0x01)==0;
  const bool mono=((header[3]>>6)&0x03)==3;
  const uint32_t sideInfo=mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
  const uint32_t infoOffset=audioStart+4+(crcPresent ? 2 : 0)+sideInfo;
  if(!file.seek(static_cast<int32_t>(infoOffset),SEEK_SET) || file.read(marker,sizeof(marker))!=sizeof(marker) ||
     (memcmp(marker,"Xing",4)!=0 && memcmp(marker,"Info",4)!=0)) {
    file.seek(static_cast<int32_t>(audioStart),SEEK_SET); return;
  }
  const uint32_t sampleRates[3][3]={{11025,12000,8000},{22050,24000,16000},{44100,48000,32000}};
  const uint16_t bitrateMpeg1[15]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
  const uint16_t bitrateMpeg2[15]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
  const uint32_t sampleRate=sampleRates[mpeg1 ? 2 : (version==2 ? 1 : 0)][rateIndex];
  const uint32_t bitrate=mpeg1 ? bitrateMpeg1[bitrateIndex] : bitrateMpeg2[bitrateIndex];
  const uint32_t frameBytes=((mpeg1 ? 144UL : 72UL)*bitrate*1000UL/sampleRate)+((header[2]>>1)&0x01);
  const uint32_t musicStart=audioStart+frameBytes;
  if(musicStart>=file.getSize() || !file.seek(static_cast<int32_t>(musicStart),SEEK_SET))
    file.seek(static_cast<int32_t>(audioStart),SEEK_SET);
}
static bool validateAndSkipId3v2(AudioFileSourceSD& file, String& error, GaplessInfo* gapless=nullptr) {
  if(gapless) *gapless=GaplessInfo();
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
    GaplessInfo ignored;
    inspectGaplessInfo(file,0,gapless ? *gapless : ignored);
    skipXingInfoFrame(file,0);
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
  GaplessInfo ignored;
  inspectGaplessInfo(file,skip,gapless ? *gapless : ignored);
  skipXingInfoFrame(file,skip);
  return true;
}
// Caller haelt Audio-Lock und SD-Lock, sofern Dateiobjekte bestehen.
static void clearPcmPrimer() {
  if(pcmDecoder) { if(pcmDecoder->isRunning()) pcmDecoder->stop(); delete pcmDecoder; pcmDecoder=nullptr; }
  delete pcmBuffer; pcmBuffer=nullptr;
  delete pcmSource; pcmSource=nullptr;
  free(pcmInputBuffer); pcmInputBuffer=nullptr;
  free(pcmWorkspace); pcmWorkspace=nullptr;
}
static void clearPcm() {
  clearPcmPrimer();
  free(pcmSamples); pcmSamples=nullptr;
  pcmPath=""; pcmBuilding=pcmReady=pcmHandoff=pcmCaughtUp=pcmCatchupEnded=false; pcmRead=0;
  pcmGapless=GaplessInfo();
  handoffDecoder=nullptr;
  pcmOutput.reset(nullptr);
}
static bool beginPcmPrimer() {
  const size_t required=cfg::MIN_HEAP_RESERVE+cfg::PCM_PREFETCH_BYTES+
                        AudioGeneratorMP3::preAllocSize()+cfg::MP3_BUFFER_BYTES;
  if(ESP.getFreeHeap()<required) return false;
  pcmSamples=static_cast<int16_t*>(malloc(cfg::PCM_PREFETCH_BYTES));
  pcmWorkspace=malloc(AudioGeneratorMP3::preAllocSize());
  pcmInputBuffer=static_cast<uint8_t*>(malloc(cfg::MP3_BUFFER_BYTES));
  pcmSource=new (std::nothrow) CheckedSource();
  if(!pcmSamples || !pcmWorkspace || !pcmInputBuffer || !pcmSource) { clearPcm(); return false; }
  pcmSource->monitor(false);
  String error;
  if(!pcmSource->open(pcmPath.c_str()) || !validateAndSkipId3v2(*pcmSource,error,&pcmGapless)) { clearPcm(); return false; }
  pcmBuffer=new (std::nothrow) AudioFileSourceBuffer(pcmSource,pcmInputBuffer,cfg::MP3_BUFFER_BYTES);
  pcmDecoder=new (std::nothrow) AudioGeneratorMP3(pcmWorkspace,AudioGeneratorMP3::preAllocSize());
  if(!pcmBuffer || !pcmDecoder) { clearPcm(); return false; }
  pcmOutput.reset(pcmSamples,pcmGapless);
  if(!pcmDecoder->begin(pcmBuffer,&pcmOutput)) { clearPcm(); return false; }
  return true;
}
static void servicePcmPrimer() {
  if(pcmBuilding || !pcmDecoder) return;
  pcmOutput.beginSlice(cfg::PCM_PREFETCH_SLICE_FRAMES);
  const bool running=pcmDecoder->loop();
  if(!pcmOutput.full() && running) return;
  if(pcmOutput.frames()>=cfg::PCM_PREFETCH_MIN_FRAMES) {
    pcmReady=true;
    clearPcmPrimer();
  } else clearPcm();
}
static void drainPcmToI2s() {
  while(pcmSamples && pcmRead<pcmOutput.frames()) {
    int16_t* sample=&pcmSamples[pcmRead*2];
    if(!output->ConsumeSample(sample)) break;
    ++pcmRead;
  }
}
static bool servicePcmHandoff() {
  drainPcmToI2s();
  if(!pcmCaughtUp && !pcmCatchupEnded) {
    const bool running=decoder->loop();
    if(catchupOutput.caughtUp()) {
      handoffDecoder->switchOutput(output); handoffDecoder=nullptr;
      output->retainI2s(false); pcmCaughtUp=true;
    } else if(!running) {
      // Sehr kurze Datei: Der PCM-Puffer reicht bis zum normalen Dateiende.
      pcmCatchupEnded=true; handoffDecoder=nullptr;
    }
  }
  if(pcmRead<pcmOutput.frames()) return true;
  if(pcmCatchupEnded) return false;
  if(!pcmCaughtUp) return true;
  free(pcmSamples); pcmSamples=nullptr; pcmPath=""; pcmReady=false; pcmHandoff=false;
  pcmOutput.reset(nullptr);
  return decoder->loop();
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
  if(!retainI2s) clearPcm();
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
        bool audioRunning=true;
        if (decoder && current.load() == State::Running && !pausedPlayback.load()) {
          audioRunning=pcmHandoff ? servicePcmHandoff() :
                                   (decoder->isRunning() && decoder->loop());
        }
        if (decoder && current.load() == State::Running && !pausedPlayback.load() && !audioRunning) {
          trace("decoder end/error detected in Audio-Task");
          const bool broken=readFailed || decoderFailed || !decoder->isRunning() || acceptedSamples.load()<=256;
          lastError=readFailed ? "SD-Lesefehler innerhalb der MP3." : "MP3-Decoderfehler / keine dekodierbaren Samples.";
          if(!broken) trace("normal end: I2S DMA tail retained for handoff");
          cleanup(!broken);
          if (readFailed && !storage::healthy()) current=State::StorageError;
          else current=broken ? State::FileError : State::Ended;
          trace("terminal audio state published");
        }
        if (decoder && current.load()==State::Running && !pausedPlayback.load() && !pcmHandoff) {
          // Kleine Decoder-Scheiben halten die Ausgabe des laufenden Titels priorisiert.
          servicePcmPrimer();
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
  // Nur der erwartete, normal beendete Folgetitel darf den PCM-Startpuffer
  // uebernehmen. Fuer jeden anderen Start bleibt die bewährte MP3-Vorladung
  // der sichere Fallback.
  const bool usePcm=handoff && pcmReady && pcmSamples &&
                    preparedSource && preparedPath==path && pcmPath==path;
  cleanup(handoff); current.store(State::Idle); acceptedSamples.store(0);
  if(!usePcm) clearPcm();
  uint32_t phaseStartedAt=millis();
  const bool usePrepared=preparedSource && preparedPath==path;
  if(usePrepared) {
    source=preparedSource; preparedSource=nullptr; preparedPath="";
    currentGapless=preparedGapless;
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
    if (!validateAndSkipId3v2(*source, error, &currentGapless)) { traceDuration("MP3 header/ID3 validation failed", phaseStartedAt); startFileFailure=true; cleanup(); return false; }
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
  decoder = usePcm
      ? static_cast<AudioGeneratorMP3*>(new (std::nothrow) HandoffDecoder(decoderWorkspace, static_cast<int>(workBytes)))
      : new (std::nothrow) AudioGeneratorMP3(decoderWorkspace, static_cast<int>(workBytes));
  if (!buffer || !decoder) { traceDuration("decoder/buffer object allocation failed", phaseStartedAt); error = "MP3-Objekt konnte nicht angelegt werden."; cleanup(); return false; }
  decoder->RegisterStatusCB(decoderStatus,nullptr);
  userVolume=userGain(percent); output->SetGain(userVolume);
  if(usePcm) output->beginTrack(currentGapless,pcmOutput.firstRawFrame(),false);
  else output->beginTrack(currentGapless);
  traceDuration("source/buffer/decoder object preparation", phaseStartedAt);
  phaseStartedAt=millis();
  if(usePcm) {
    handoffDecoder=static_cast<HandoffDecoder*>(decoder);
    catchupOutput.reset(pcmOutput.rawFrames());
    if(pcmOutput.rate()>0) output->SetRate(pcmOutput.rate());
    if (!decoder->begin(buffer, &catchupOutput)) { traceDuration("PCM decoder->begin failed", phaseStartedAt); error = "MP3-Decoder/I2S konnte nicht starten."; cleanup(); return false; }
    pcmReady=false; pcmHandoff=true; pcmCaughtUp=false; pcmCatchupEnded=false;
    traceDuration("PCM handoff decoder->begin", phaseStartedAt);
  } else if (!decoder->begin(buffer, output)) {
    traceDuration("decoder->begin failed", phaseStartedAt); error = "MP3-Decoder/I2S konnte nicht starten."; cleanup(); return false;
  }
  traceDuration("decoder->begin", phaseStartedAt);
  if(!usePcm) output->retainI2s(false);
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
  GaplessInfo candidateGapless;
  {
    storage::Guard sdLock;
    candidate=new (std::nothrow) CheckedSource();
    if(!candidate) { error="MP3-Vorbereitung: Speicher fehlt."; return false; }
    candidate->monitor(false);
    if(!candidate->open(path.c_str())) {
      error="MP3-Vorbereitung: Datei nicht geoeffnet."; delete candidate; return false;
    }
    if(!validateAndSkipId3v2(*candidate,error,&candidateGapless)) {
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
    clearPcm();
    preparedSource=candidate; preparedPath=path; preparedGapless=candidateGapless;
    // Die zweite Datei wird ausserhalb des Audio-Locks geoeffnet. So kann eine
    // langsame SD-Antwort nicht den laufenden Audio-Task anhalten.
    pcmPath=path; pcmBuilding=true;
  }
  bool pcmStarted=false;
  {
    storage::Guard sdLock;
    pcmStarted=beginPcmPrimer();
  }
  {
    Lock audioLock;
    pcmBuilding=false; // Erst jetzt darf der Audio-Task den Decoder verwenden.
  }
  if(!pcmStarted) trace("PCM primer unavailable; compressed MP3 preload remains active");
  return true;
}
void discardPrepared() {
  if(!engineMutex) return;
  Lock audioLock; storage::Guard sdLock;
  clearPrepared();
  clearPcm();
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

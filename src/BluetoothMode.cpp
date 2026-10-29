#include "BluetoothMode.h"
#include "BtDevices.h"
#include "Config.h"
#include "BtTransport.h"
#include <BluetoothA2DPSink.h>
#include <esp_bt_main.h>
#include <atomic>

namespace bluetooth {
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
struct Policy {
  bool ready=false, locked=false, pair=false, shutting=false;
  uint8_t allowed[6]={}, peer[6]={}, authenticated[6]={};
  uint8_t bonds[btdevices::MAX_DEVICES][6]={}; size_t count=0;
  uint8_t namedMac[6]={};
  char name[ESP_BT_GAP_MAX_BDNAME_LEN+1]={}, peerName[ESP_BT_GAP_MAX_BDNAME_LEN+1]={};
  bool namePending=false;
};
static Policy policy;
static std::atomic<int> connection{ESP_A2D_CONNECTION_STATE_DISCONNECTED};
static std::atomic<bool> audioAllowed{false}, stackReady{false}, refreshBonds{false};
static std::atomic<bool> avrcReady{false}, a2dpStarted{false};
static std::atomic<bt_transport::Playback> avrcPlayback{bt_transport::Playback::Unknown};
static std::atomic<uint32_t> playbackRevision{0};
static std::atomic<uint32_t> lastSignal{0}, signalFrames{0}, sampleRate{44100};
static uint32_t pairStart=0, nextReconnect=0, lastSync=0;
static unsigned attempts=0;
static bool pairOpen=false, locked=false, shutdownStarted=false;
static String deviceName;
static int heldCommand=-1;
static uint32_t releaseAt=0, optimisticAt=0, optimisticRevision=0;
static bt_transport::Playback optimisticPlayback=bt_transport::Playback::Unknown;
static void playbackChanged(esp_avrc_playback_stat_t state) {
  using bt_transport::Playback;
  Playback value=Playback::Unknown;
  if(state==ESP_AVRC_PLAYBACK_PLAYING || state==ESP_AVRC_PLAYBACK_FWD_SEEK || state==ESP_AVRC_PLAYBACK_REV_SEEK)
    value=Playback::Playing;
  else if(state==ESP_AVRC_PLAYBACK_PAUSED || state==ESP_AVRC_PLAYBACK_STOPPED)
    value=Playback::Paused;
  avrcPlayback.store(value); playbackRevision.fetch_add(1);
}
static void streamChanged(esp_a2d_audio_state_t state,void*) {
  a2dpStarted.store(state==ESP_A2D_AUDIO_STATE_STARTED); playbackRevision.fetch_add(1);
}
static void remoteChanged(bool connected) {
  avrcReady.store(connected);
  if(!connected) { avrcPlayback.store(bt_transport::Playback::Unknown); playbackRevision.fetch_add(1); }
}
static void releaseCommand(bool immediate=false) {
  if(heldCommand<0) return;
  if(!immediate && static_cast<int32_t>(millis()-releaseAt)<0) return;
  if(avrcReady.load() && connection.load()==ESP_A2D_CONNECTION_STATE_CONNECTED)
    esp_avrc_ct_send_passthrough_cmd(0,heldCommand,ESP_AVRC_PT_CMD_STATE_RELEASED);
  heldCommand=-1;
}
static bool sendCommand(uint8_t command) {
  if(!stackReady.load() || shutdownStarted || !avrcReady.load() || !audioAllowed.load() ||
     connection.load()!=ESP_A2D_CONNECTION_STATE_CONNECTED || heldCommand>=0) return false;
  const esp_err_t result=esp_avrc_ct_send_passthrough_cmd(0,command,ESP_AVRC_PT_CMD_STATE_PRESSED);
  if(result!=ESP_OK) { Serial.printf("[BT] AVRCP-Befehl fehlgeschlagen: %d\n",result); return false; }
  heldCommand=command; releaseAt=millis()+100; return true;
}
static bool knownLocked(const uint8_t* mac) {
  for(size_t i=0;i<policy.count;++i) if(btdevices::equal(policy.bonds[i],mac)) return true;
  return false;
}
static bool allowedLocked(const uint8_t* mac) {
  if(!policy.ready || policy.shutting) return false;
  if(policy.locked && !btdevices::equal(mac,policy.allowed)) return false;
  return knownLocked(mac) || (policy.pair && btdevices::equal(mac,policy.authenticated));
}
static bool validate(esp_bd_addr_t mac) {
  portENTER_CRITICAL(&mux); const bool ok=allowedLocked(mac); portEXIT_CRITICAL(&mux);
  return ok;
}
static bool pairingPermitted() {
  portENTER_CRITICAL(&mux); const bool ok=policy.ready && policy.pair && !policy.locked && !policy.shutting;
  portEXIT_CRITICAL(&mux); return ok;
}
// Keine NVS-/Dateizugriffe aus GAP oder PCM. Hauptschleife uebernimmt Ereignisse.
class Receiver : public BluetoothA2DPSink {
 public:
  Receiver() { discoverability=ESP_BT_NON_DISCOVERABLE; }
  void finishShutdown() {
    esp_a2d_sink_deinit(); esp_avrc_ct_deinit(); esp_avrc_tg_deinit();
    if(esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_ENABLED) esp_bluedroid_disable();
    if(esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_INITIALIZED) esp_bluedroid_deinit();
    if(esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_ENABLED) esp_bt_controller_disable();
    app_task_shut_down(); if(is_output) out->end();
  }
 protected:
  // Bibliotheks-NVS absichtlich deaktiviert: nur bestaetigte Musikwiedergabe
  // bestimmt unsere bevorzugte Quelle; alte last_bda-Eintraege werden ignoriert.
  void get_last_connection() override { memset(last_connection,0,6); }
  void set_last_connection(esp_bd_addr_t mac) override { memcpy(last_connection,mac,6); }
  void av_hdl_stack_evt(uint16_t event,void* data) override {
    BluetoothA2DPSink::av_hdl_stack_evt(event,data); stackReady=true;
  }
  void app_gap_callback(esp_bt_gap_cb_event_t event,esp_bt_gap_cb_param_t* p) override {
    if(event==ESP_BT_GAP_CFM_REQ_EVT) {
      // Auch eine bekannte MAC darf ausserhalb des Fensters KEINEN neuen Key aushandeln.
      esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda,pairingPermitted()); return;
    }
    if(event==ESP_BT_GAP_PIN_REQ_EVT) {
      esp_bt_pin_code_t pin={'0','0','0','0'};
      esp_bt_gap_pin_reply(p->pin_req.bda,pairingPermitted() && !p->pin_req.min_16_digit,4,pin); return;
    }
    if(event==ESP_BT_GAP_KEY_REQ_EVT) { esp_bt_gap_ssp_passkey_reply(p->key_req.bda,false,0); return; }
    if(event==ESP_BT_GAP_AUTH_CMPL_EVT) {
      portENTER_CRITICAL(&mux);
      const bool permitted=policy.ready && !policy.shutting &&
        (!policy.locked || btdevices::equal(policy.allowed,p->auth_cmpl.bda)) &&
        (knownLocked(p->auth_cmpl.bda) || policy.pair);
      if(p->auth_cmpl.stat==ESP_BT_STATUS_SUCCESS && permitted) {
        memcpy(policy.authenticated,p->auth_cmpl.bda,6);
        memcpy(policy.namedMac,p->auth_cmpl.bda,6);
        memcpy(policy.name,p->auth_cmpl.device_name,ESP_BT_GAP_MAX_BDNAME_LEN);
        policy.name[ESP_BT_GAP_MAX_BDNAME_LEN]=0; policy.namePending=true;
      }
      portEXIT_CRITICAL(&mux);
      if(p->auth_cmpl.stat!=ESP_BT_STATUS_SUCCESS || !permitted) esp_a2d_sink_disconnect(p->auth_cmpl.bda);
      refreshBonds=true;
    } else if(event==ESP_BT_GAP_READ_REMOTE_NAME_EVT && p->read_rmt_name.stat==ESP_BT_STATUS_SUCCESS) {
      portENTER_CRITICAL(&mux);
      memcpy(policy.namedMac,p->read_rmt_name.bda,6);
      memcpy(policy.name,p->read_rmt_name.rmt_name,ESP_BT_GAP_MAX_BDNAME_LEN);
      policy.name[ESP_BT_GAP_MAX_BDNAME_LEN]=0; policy.namePending=true;
      portEXIT_CRITICAL(&mux);
    } else if(event==ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT) refreshBonds=true;
    BluetoothA2DPSink::app_gap_callback(event,p);
  }
  void handle_connection_state(uint16_t event,void* data) override {
    auto* p=static_cast<esp_a2d_cb_param_t*>(data);
    if(p->conn_stat.state==ESP_A2D_CONNECTION_STATE_CONNECTED) {
      const bool ok=validate(p->conn_stat.remote_bda);
      portENTER_CRITICAL(&mux);
      memcpy(policy.peer,p->conn_stat.remote_bda,6);
      policy.peerName[0]=0;
      if(btdevices::equal(policy.namedMac,policy.peer)) memcpy(policy.peerName,policy.name,sizeof(policy.peerName));
      portEXIT_CRITICAL(&mux);
      audioAllowed=ok; signalFrames=0; lastSignal=0;
    } else if(p->conn_stat.state==ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
      audioAllowed=false; signalFrames=0; lastSignal=0;
      a2dpStarted=false; avrcPlayback=bt_transport::Playback::Unknown; playbackRevision.fetch_add(1);
      portENTER_CRITICAL(&mux); memset(policy.authenticated,0,6); policy.peerName[0]=0; portEXIT_CRITICAL(&mux);
    }
    BluetoothA2DPSink::handle_connection_state(event,data);
    connection=p->conn_stat.state;
  }
  void audio_data_callback(const uint8_t* bytes,uint32_t len) override {
    if(!audioAllowed.load()) return;
    bool signal=false;
    for(uint32_t i=0;i+1<len;i+=2) {
      const int16_t s=static_cast<int16_t>(uint16_t(bytes[i])|(uint16_t(bytes[i+1])<<8));
      if(s>8 || s<-8) { signal=true; break; }
    }
    if(signal) {
      const uint32_t now=millis(), previous=lastSignal.exchange(now);
      if(!previous || uint32_t(now-previous)>300) signalFrames=0;
      if(signalFrames.load()<sampleRate.load()*2) signalFrames.fetch_add(len/4);
    }
    BluetoothA2DPSink::audio_data_callback(bytes,len);
  }
};
static Receiver* receiver=nullptr;
static void synchronize() {
  esp_bd_addr_t bonds[btdevices::MAX_DEVICES];
  int count=esp_bt_gap_get_bond_device_num();
  if(count<0 || count>int(btdevices::MAX_DEVICES)) {
    portENTER_CRITICAL(&mux); policy.ready=false; portEXIT_CRITICAL(&mux);
    lastSync=millis(); Serial.println("[BT] Bondliste zu gross/unlesbar; neue Verbindungen bleiben gesperrt."); return;
  }
  if(count && esp_bt_gap_get_bond_device_list(&count,bonds)!=ESP_OK) { lastSync=millis(); return; }
  bool deleting=false;
  for(int i=0;i<count;++i) if(btdevices::pending(bonds[i])) {
    esp_bt_gap_remove_bond_device(bonds[i]); deleting=true;
  }
  btdevices::reconcile(bonds,count);
  portENTER_CRITICAL(&mux);
  policy.count=0;
  for(int i=0;i<count;++i) if(!btdevices::pending(bonds[i])) memcpy(policy.bonds[policy.count++],bonds[i],6);
  policy.ready=!deleting && !btdevices::pendingAll();
  portEXIT_CRITICAL(&mux);
  lastSync=millis();
}
bool begin(const String& name) {
  btdevices::begin(); deviceName=name;
  static Receiver localReceiver; receiver=&localReceiver;
  i2s_pin_config_t pins={}; pins.mck_io_num=I2S_PIN_NO_CHANGE;
  pins.bck_io_num=cfg::I2S_BCK; pins.ws_io_num=cfg::I2S_LCK;
  pins.data_out_num=cfg::I2S_DOUT; pins.data_in_num=I2S_PIN_NO_CHANGE;
  receiver->set_pin_config(pins); receiver->set_address_validator(validate);
  receiver->set_avrc_connection_state_callback(remoteChanged);
  receiver->set_avrc_rn_playstatus_callback(playbackChanged);
  receiver->set_on_audio_state_changed(streamChanged);
  receiver->set_sample_rate_callback([](uint16_t rate){ sampleRate=rate; });
  receiver->set_auto_reconnect(false); receiver->activate_pin_code(true);
  receiver->set_avrc_metadata_attribute_mask(0); receiver->set_volume(64);
  receiver->start(deviceName.c_str()); nextReconnect=millis()+1500;
  return esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_ENABLED;
}
void pairing() {
  if(!receiver || locked || shutdownStarted) return;
  releaseCommand(true);
  pairOpen=true; pairStart=millis(); attempts=0;
  portENTER_CRITICAL(&mux); policy.pair=true; uint8_t peer[6]; memcpy(peer,policy.peer,6); portEXIT_CRITICAL(&mux);
  if(connected()) esp_a2d_sink_disconnect(peer);
  receiver->set_discoverability(ESP_BT_GENERAL_DISCOVERABLE); receiver->set_connectable(true);
}
void togglePlayback() {
  using bt_transport::Playback;
  const uint32_t revision=playbackRevision.load();
  Playback state=avrcPlayback.load();
  if(optimisticPlayback!=Playback::Unknown && revision==optimisticRevision &&
     uint32_t(millis()-optimisticAt)<1500) state=optimisticPlayback;
  const bool pause=bt_transport::shouldPause(state,a2dpStarted.load());
  if(sendCommand(pause?ESP_AVRC_PT_CMD_PAUSE:ESP_AVRC_PT_CMD_PLAY)) {
    optimisticPlayback=pause?Playback::Paused:Playback::Playing;
    optimisticRevision=revision; optimisticAt=millis();
  }
}
void nextTrack() { sendCommand(ESP_AVRC_PT_CMD_FORWARD); }
void previousTrack() { sendCommand(ESP_AVRC_PT_CMD_BACKWARD); }
void setLocked(bool enabled) {
  if(!receiver || enabled==locked) return;
  locked=enabled; uint8_t allowed[6]={};
  if(enabled) {
    if(connected() && audioAllowed.load()) { portENTER_CRITICAL(&mux); memcpy(allowed,policy.peer,6); portEXIT_CRITICAL(&mux); }
    else btdevices::preferred(allowed);
  }
  portENTER_CRITICAL(&mux); policy.locked=enabled; memcpy(policy.allowed,allowed,6); policy.pair=false; portEXIT_CRITICAL(&mux);
  pairOpen=false; attempts=0; nextReconnect=millis()+1000;
  receiver->set_discoverability(ESP_BT_NON_DISCOVERABLE);
}
void tick() {
  releaseCommand();
  if(!receiver || !stackReady.load() || shutdownStarted) return;
  const uint32_t now=millis();
  if(refreshBonds.exchange(false) || uint32_t(now-lastSync)>2000 || lastSync==0) synchronize();
  uint8_t named[6]; char name[ESP_BT_GAP_MAX_BDNAME_LEN+1]; bool namePending;
  portENTER_CRITICAL(&mux); namePending=policy.namePending; policy.namePending=false;
  memcpy(named,policy.namedMac,6); memcpy(name,policy.name,sizeof(name)); portEXIT_CRITICAL(&mux);
  if(namePending) {
    btdevices::remember(named,name); btdevices::save();
    portENTER_CRITICAL(&mux);
    if(btdevices::equal(named,policy.peer) && name[0]) memcpy(policy.peerName,name,sizeof(policy.peerName));
    portEXIT_CRITICAL(&mux);
  }
  if(connected() && audioAllowed.load() && signalFrames.load()>=sampleRate.load()) {
    uint8_t peer[6]; portENTER_CRITICAL(&mux); memcpy(peer,policy.peer,6); portEXIT_CRITICAL(&mux);
    btdevices::confirm(peer);
  }
  if(pairOpen && uint32_t(now-pairStart)>=cfg::PAIRING_MS) {
    pairOpen=false; portENTER_CRITICAL(&mux); policy.pair=false; portEXIT_CRITICAL(&mux);
    receiver->set_discoverability(ESP_BT_NON_DISCOVERABLE);
  }
  if(connected()) { attempts=0; nextReconnect=now+10000; return; }
  // Auch nach Verbindungsverlust hoechstens drei eigene Versuche; Bibliotheks-Retry aus.
  if(connection.load()==ESP_A2D_CONNECTION_STATE_DISCONNECTED && !pairOpen && attempts<3 && int32_t(now-nextReconnect)>=0) {
    uint8_t target[6]={};
    if(locked) { portENTER_CRITICAL(&mux); memcpy(target,policy.allowed,6); portEXIT_CRITICAL(&mux); }
    else btdevices::preferred(target);
    if(validate(target)) { receiver->connect_to(target); ++attempts; }
    nextReconnect=now+10000;
  }
}
bool connected() { return connection.load()==ESP_A2D_CONNECTION_STATE_CONNECTED; }
String connectedDeviceName() {
  if(!connected() || !audioAllowed.load()) return "";
  uint8_t peer[6]; char name[ESP_BT_GAP_MAX_BDNAME_LEN+1];
  portENTER_CRITICAL(&mux); memcpy(peer,policy.peer,6); memcpy(name,policy.peerName,sizeof(name)); portEXIT_CRITICAL(&mux);
  if(name[0]) return String(name);
  // Bekannten Namen direkt nach Reconnect nutzen, bevor GAP erneut antwortet.
  for(size_t i=0;i<btdevices::count();++i) {
    const btdevices::Device& device=btdevices::at(i);
    if(btdevices::equal(peer,device.mac) && device.name[0]) return String(device.name);
  }
  return btdevices::format(peer);
}
bool streaming() { const uint32_t last=lastSignal.load(); return audioAllowed.load() && last && uint32_t(millis()-last)<1500; }
bool pairingActive() { return pairOpen; }
uint32_t pairingSeconds() { const uint32_t elapsed=millis()-pairStart; return pairOpen && elapsed<cfg::PAIRING_MS ? (cfg::PAIRING_MS-elapsed+999)/1000:0; }
void beforeRestart() {
  if(!receiver || shutdownStarted) return;
  releaseCommand(true);
  shutdownStarted=true; pairOpen=false; audioAllowed=false;
  uint8_t peer[6]; portENTER_CRITICAL(&mux); policy.shutting=true; policy.pair=false; memcpy(peer,policy.peer,6); portEXIT_CRITICAL(&mux);
  receiver->set_connectable(false); receiver->set_discoverability(ESP_BT_NON_DISCOVERABLE);
  if(connected()) esp_a2d_sink_disconnect(peer);
  btdevices::save();
}
void shutdown() { if(receiver) receiver->finishShutdown(); }
}

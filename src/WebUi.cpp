#include "WebUi.h"
#include "App.h"
#include "Config.h"
#include "Rules.h"
#include "AudioEngine.h"
#include "Controls.h"
#include "BtDevices.h"
#include "FileIssues.h"
#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <freertos/task.h>
#include <atomic>

namespace webui {
static WebServer server(80);
static String csrf;
static std::atomic<bool> apHidden{false}, running{false};
static bool hideTaskStarted = false;
static uint32_t apStarted = 0, lastHideTry = 0;
static String html(const String& s) { return String(rules::html(s.c_str()).c_str()); }
static String url(const String& s) { return String(rules::url(s.c_str()).c_str()); }
static String json(const String& s) { return String(rules::json(s.c_str()).c_str()); }
static String pageUrl(const String& list) { return list.isEmpty() ? String("/motion") : "/playlist?list=" + url(list); }
static void headers() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("Referrer-Policy", "no-referrer");
}
static void fail(int code, const String& reason) { headers(); server.send(code, "text/plain; charset=utf-8", reason); }
static void redirect(const String& to = "/") {
  headers();
  if(server.header("X-Requested-With")=="fetch") { server.send(200,"application/json","{\"ok\":true}"); return; }
  server.sendHeader("Location", to, true); server.send(303, "text/plain", "");
}
static bool authorized() {
  if (server.arg("token") != csrf) { fail(403, "Ungueltiger Sitzungsschluessel. Seite nach Neustart neu laden."); return false; }
  if(app::busy()) { fail(409,"Geraet fuehrt gerade eine andere Aktion aus."); return false; }
  if(controls::fullyLocked()) { fail(423,"Tasten gesperrt. Am Geraet UP+DOWN halten."); return false; }
  app::activity();
  return true;
}
static bool admin() {
  if(!server.authenticate(cfg::BT_ADMIN_USER,cfg::BT_ADMIN_PASSWORD)) {
    server.requestAuthentication(DIGEST_AUTH,"AudioESP32 Verwaltung","Admin-Zugang erforderlich."); return false;
  }
  return true;
}
static bool validList(const String& list, bool allowMotion = true) {
  if (list.isEmpty()) return allowMotion;
  return rules::playlistName(list.c_str()) && app::playlistIndex(list) >= 0;
}
static String hiddenField(const char* name, const String& value) {
  return "<input type='hidden' name='" + String(name) + "' value='" + html(value) + "'>";
}
static String formStart(const char* action, const String& list = "") {
  return "<form method='post' action='" + String(action) + "'>" + hiddenField("token", csrf) + hiddenField("list", list);
}
static void beginPage(const String& title) {
  app::activity();
  headers(); server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  server.sendContent(R"HTML(<!doctype html><html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Audio-ESP32</title>
<style>
:root{font-family:system-ui,sans-serif;color:#172734;background:#edf1f5}*{box-sizing:border-box}
body{max-width:1060px;margin:auto;padding:20px 14px 50px}a{color:#12616c}h1{font-size:1.6rem}h2{font-size:1.2rem}
section{background:white;border:1px solid #d6dfe6;border-radius:10px;padding:16px;margin:16px 0}
table{border-collapse:collapse;width:100%}td,th{padding:9px 6px;text-align:left;border-bottom:1px solid #dce4ea;overflow-wrap:anywhere}
input,button{font:inherit;min-height:44px}input[type=text],input[type=number]{padding:7px;border:1px solid #afbdc9;border-radius:5px;max-width:100%}
input[type=number]{width:100px}button,.button{display:inline-block;background:#146a76;color:white;border:0;border-radius:5px;padding:8px 11px;cursor:pointer;text-decoration:none;margin:3px 0}
.danger{background:#9c3541}.muted{color:#5c6b76;font-size:.9rem}.warning{background:#fff0cb;padding:12px;border-radius:6px}
.status{background:#e8f2f3;padding:12px;white-space:pre-line}form.inline{display:inline}label{display:inline-block;margin:6px 12px 6px 0}
.scroll{overflow-x:auto}.row{display:flex;gap:12px;align-items:center;flex-wrap:wrap}#uploadState{white-space:pre-line}
input[type=range]{width:min(100%,360px);vertical-align:middle}input[type=checkbox]{min-height:24px;min-width:24px;vertical-align:middle}
@media(max-width:600px){body{padding:10px 8px}section{padding:12px}table{font-size:.9rem}button,.button{min-height:44px}nav{line-height:2.5}}
</style></head><body><nav><a href="/">Start</a> &middot; <a href="/motion">Bewegungsmelder</a> &middot; <a href="/bluetooth">Bluetooth-Geraete</a> &middot; <a href="/errors">Fehlerhafte Dateien</a></nav>)HTML");
  server.sendContent("<h1>" + html(title) + "</h1><div id='live' class='status'>Status laden ...</div>");
  if (!app::note.isEmpty()) server.sendContent("<p class='warning'>" + html(app::note) + "</p>");
}
static void endPage() {
  server.sendContent(R"HTML(<script>
async function statusUpdate(){try{
const r=await fetch('/status',{cache:'no-store'});if(!r.ok)return;const s=await r.json();
document.querySelector('#live').textContent='Modus: '+s.mode+' | PIR: '+(s.pir?'AN':'AUS')+
'\nWiedergabe: '+(s.playing?(s.paused?'Pause: ':'')+s.song:'gestoppt')+(s.playlist?' | Playlist: '+s.playlist:'')+
(s.next?'\nAls Naechstes: '+s.next:'')+
'\nSD: '+(s.sd?'bereit':'nicht erkannt')+' | Heap: '+s.heap+' Bytes | WLAN: '+(s.hidden?'versteckt':'sichtbar')+(s.note?'\nHinweis: '+s.note:'');
}catch(e){document.querySelector('#live').textContent='Nicht erreichbar (Bluetooth-Modus, Neustart oder WLAN getrennt).';}}
statusUpdate();setInterval(statusUpdate,3000);
document.querySelectorAll('form.delete').forEach(f=>f.addEventListener('submit',e=>{if(!confirm('Diesen Titel dauerhaft von der SD-Karte loeschen?'))e.preventDefault();}));
const upload=document.querySelector('#uploadForm');if(upload)upload.addEventListener('submit',e=>{
e.preventDefault();const file=document.querySelector('#mp3file').files[0];if(!file)return;
const msg=document.querySelector('#uploadState'),btn=document.querySelector('#uploadButton');
const xhr=new XMLHttpRequest();xhr.open('POST',upload.action);btn.disabled=true;
xhr.upload.onprogress=e=>{if(e.lengthComputable)msg.textContent='Upload: '+Math.round(100*e.loaded/e.total)+' %';};
xhr.onload=()=>{btn.disabled=false;if(xhr.status===200){msg.textContent='Gespeichert.';location.reload();}else msg.textContent='Fehler: '+xhr.responseText;};
xhr.onerror=()=>{btn.disabled=false;msg.textContent='Netzwerkfehler. Seite neu laden und SD-Inhalt pruefen.';};
const data=new FormData();data.append('file',file,file.name);msg.textContent='Upload beginnt, SD-Wiedergabe wird gestoppt.';xhr.send(data);
});
const volume=document.querySelector('#volume');if(volume)volume.addEventListener('input',()=>document.querySelector('#volumeValue').textContent=volume.value+' %');
async function post(action,data){const r=await fetch(action,{method:'POST',headers:{'X-Requested-With':'fetch'},body:data});if(!r.ok)throw Error(await r.text());statusUpdate();}
document.querySelectorAll('form[action="/settings"],form[action="/control"],form[action="/track"]').forEach(f=>f.addEventListener('submit',async e=>{
e.preventDefault();const d=new FormData(f);if(e.submitter&&e.submitter.name)d.append(e.submitter.name,e.submitter.value);
try{await post(f.action,d);}catch(error){alert(error.message);}}));
if(volume){let timer;const sendVolume=persist=>{const d=new FormData();d.append('token',volume.form.elements.token.value);d.append('volume',volume.value);d.append('persist',persist?'1':'0');post('/volume',d).catch(e=>{if(persist)alert(e.message);});};
volume.addEventListener('input',()=>{clearTimeout(timer);timer=setTimeout(()=>sendVolume(false),120);});
volume.addEventListener('change',()=>{clearTimeout(timer);sendVolume(true);});}
document.querySelectorAll('form.confirm').forEach(f=>f.addEventListener('submit',e=>{if(!confirm(f.dataset.confirm||'Diese Aktion wirklich ausfuehren?'))e.preventDefault();}));
</script><p class="muted">Audio-ESP32 V2 &middot; Nur lokaler Zugriff. Im Bluetooth-Modus ist WLAN ausgeschaltet.</p></body></html>)HTML");
  server.sendContent("");
}
static void home() {
  beginPage("Audio-ESP32");
  server.sendContent("<section><h2>Wiedergabe</h2>"+formStart("/control")+"<button name='action' value='back'>Zurueck</button> <button name='action' value='pause'>Pause / Fortsetzen</button> <button name='action' value='next'>Weiter</button> <button name='action' value='stop'>Stop</button></form></section>");
  server.sendContent("<section><h2>Betriebsmodus</h2>" + formStart("/mode"));
  server.sendContent("<button name='mode' value='0'>Bewegungsmelder</button> <button name='mode' value='1'>Playlist</button> <button name='mode' value='2'>Bluetooth</button></form><p class='muted'>Wechsel zu/von Bluetooth startet den ESP kontrolliert neu. Rueckweg aus Bluetooth: MODE-Taster.</p></section>");
  server.sendContent("<section><h2>Playlists</h2><p>Shuffle, Endlosschleife und lokale Sichtbarkeit koennen hier direkt geaendert werden. Versteckte Playlists bleiben im Web bedienbar. Loeschen ganzer Ordner nur am PC.</p><div class='scroll'><table><thead><tr><th>Playlist</th><th>Details</th><th>Shuffle</th><th>Wiederholen</th><th>Lokal</th><th>Einstellungen</th><th>Start</th></tr></thead><tbody>");
  for (const Playlist& p : app::playlists) {
    String row = "<tr><td>" + html(p.name) + "</td><td><a class='button' href='" + pageUrl(p.name) + "'>Details</a></td><td>";
    row += p.shuffle ? "Ja" : "Nein"; row += "</td><td>";
    row += p.repeat ? "Ja" : "Nein"; row += "</td><td>";
    row += p.hidden ? "Versteckt" : "Sichtbar"; row += "</td><td>";
    row += formStart("/config", p.name);
    row += "<label><input type='checkbox' name='shuffle' value='1'" + String(p.shuffle ? " checked" : "") + "> Shuffle</label>";
    row += "<label><input type='checkbox' name='repeat' value='1'" + String(p.repeat ? " checked" : "") + "> Endlosschleife</label>";
    row += "<label><input type='checkbox' name='hidden' value='1'" + String(p.hidden ? " checked" : "") + "> Versteckt</label>";
    row += "<button>Speichern</button></form></td><td>";
    row += formStart("/play", p.name) + "<button>Abspielen</button></form></td></tr>";
    server.sendContent(row); delay(1);
  }
  if (app::playlists.empty()) server.sendContent("<tr><td colspan='7'>Keine gueltigen Playlistordner vorhanden.</td></tr>");
  server.sendContent("</tbody></table></div>" + formStart("/stop") + "<button class='danger'>Playlist stoppen</button></form></section>");
  server.sendContent("<section><h2>Neue Playlist anlegen</h2>" + formStart("/create") +
    "<input name='name' type='text' maxlength='16' placeholder='z.B. Abendmusik' required> <button>Anlegen</button></form><p class='muted'>1-16 ASCII-Zeichen: Buchstaben, Ziffern, Leerzeichen sowie _ - ( ) . , . Kein Leerzeichen/Punkt am Rand. Keine Umlaute im Ordnernamen, damit jedes unterstuetzte LCD ihn darstellen kann.</p></section>");
  server.sendContent("<section><h2>Geraeteeinstellungen</h2>" + formStart("/settings") +
    "<p><label>PIR-Zusatznachlauf (0-600 s): <input name='delay' type='number' min='0' max='600' required value='" + String(app::offDelaySeconds) + "'></label></p>" +
    "<p><label>SD-Lautstaerke: <input id='volume' name='volume' type='range' min='0' max='100' value='" + String(app::sdVolume) + "'> <span id='volumeValue'>" + String(app::sdVolume) + " %</span></label></p>" +
    "<p><label>Bluetooth-Geraetename: <input name='btname' type='text' required value='" + html(app::btName) + "'></label></p>" +
    "<p><label>Automatischer Deep Sleep (0-240 Minuten; 0 = aus): <input name='sleep' type='number' min='0' max='240' required value='"+String(app::sleepMinutes)+"'></label></p>"+
    "<button>Speichern und anwenden</button></form><p class='muted'>Bluetooth-Name: maximal 32 UTF-8-Bytes, wirksam beim naechsten BT-Start. Bluetooth-Lautstaerke am Smartphone. SD-Lautstaerke wirkt sofort; gespeichert beim Loslassen. Quadratische Lautstaerkekennlinie.</p></section>");
  server.sendContent("<section><h2>Diagnose / SD neu einlesen</h2><p>UTF-8/LFN in dieser Firmware: <strong>" + String(storage::utf8Configured() ? "aktiviert" : "NICHT aktiviert") + "</strong>. Unicode-Test und Neueinlesen stoppen SD-Musik.</p>" +
    formStart("/unicode") + "<button>Unicode-Dateizugriff testen</button></form>" +
    formStart("/rescan") + "<button>SD-Verzeichnisse neu einlesen</button></form></section>");
  endPage();
}
static void bluetoothPage() {
  if(!admin()) return;
  beginPage("Bluetooth-Geraete");
  bool pending=btdevices::pendingAll();
  for(size_t i=0;i<btdevices::count();++i) pending=pending || btdevices::at(i).pending;
  server.sendContent(pending?"<p class='warning'>Wird beim naechsten Bluetooth-Start ausgefuehrt: Loeschauftrag ausstehend.</p>":
    "<p>Keine Loeschauftraege ausstehend. Zuvor vorgemerkte Aenderungen sind abgeschlossen.</p>");
  server.sendContent("<section><p>Gespeicherter Stand vom letzten Bluetooth-Betrieb. Beim naechsten BT-Start erfolgt ein Abgleich mit den echten Kopplungsschluesseln.</p><div class='scroll'><table><tr><th>Name / MAC</th><th>Status</th><th>Aktion</th></tr>");
  uint8_t preferred[6]; const bool hasPreferred=btdevices::preferred(preferred);
  for(size_t i=0;i<btdevices::count();++i) {
    const auto& device=btdevices::at(i); const String mac=btdevices::format(device.mac);
    String row="<tr><td>"+html(device.name[0]?String(device.name):String("Name unbekannt"))+"<br>"+mac+"</td><td>";
    const bool pending=device.pending || btdevices::pendingAll();
    row+=pending?"Loeschung beim naechsten BT-Start ausstehend":"Gekoppelt (gespeicherter Stand)";
    if(hasPreferred && btdevices::equal(preferred,device.mac)) row+="<br>Zuletzt fuer Musik verwendet / bevorzugtes Reconnect-Geraet";
    row+="</td><td><form class='confirm' data-confirm='Dieses Bluetooth-Geraet entkoppeln?' method='post' action='/bt-delete'>"+hiddenField("token",csrf)+hiddenField("mac",mac)+"<button class='danger'>Geraet entfernen</button></form></td></tr>";
    server.sendContent(row);
  }
  server.sendContent("</table></div><form class='confirm' data-confirm='Wirklich ALLE Bluetooth-Kopplungen loeschen?' method='post' action='/bt-delete-all'>"+hiddenField("token",csrf)+"<p><label><input type='checkbox' name='confirm' value='yes' required> Alle Kopplungen dauerhaft entfernen</label></p><button class='danger'>Alle Kopplungen loeschen</button></form><p>Loeschauftraege bleiben bis zur bestaetigten Entfernung im Bluetooth-Stack vorgemerkt. Eine leere Liste nach dem Abgleich bedeutet: Aenderung abgeschlossen.</p></section>");
  endPage();
}
static void errorsPage() {
  beginPage("Fehlerhafte Dateien");
  server.sendContent("<section><p>Diese Titel sind von der automatischen Auswahl ausgeschlossen. Zeiten sind Sekunden seit dem angegebenen Boot (keine Echtzeituhr). Maximal 64 Eintraege; bei voller Liste wird die automatische Titelauswahl vorsorglich ausgesetzt.</p><div class='scroll'><table><tr><th>Datei / Verzeichnis</th><th>Fehler</th><th>Zeit / Versuche</th><th>Aktionen</th></tr>");
  for(const auto& e:issues::list()) {
    server.sendContent("<tr><td>"+html(e.path)+"</td><td>"+html(e.reason)+"</td><td>Boot "+String(e.boot)+", +"+String(e.seconds)+" s / "+String(e.attempts)+"</td><td>"+
      formStart("/error-retry")+hiddenField("path",e.path)+"<button>Erneut pruefen</button></form>"+
      formStart("/error-clear")+hiddenField("path",e.path)+"<button>Fehlerstatus zuruecksetzen</button></form></td></tr>");
  }
  server.sendContent("</table></div><p>Erneut pruefen testet Lesbarkeit und MP3-Header. Die Decoderpruefung erfolgt beim naechsten Abspielen; eine vollstaendige Dateipruefung wuerde die Datei komplett dekodieren.</p><form class='confirm' data-confirm='Alle Dateifehler zuruecksetzen und Titel wieder freigeben?' method='post' action='/error-clear'>"+hiddenField("token",csrf)+hiddenField("all","yes")+"<button>Gesamte Fehlerliste zuruecksetzen</button></form></section>");
  endPage();
}
static void libraryPage(const String& list) {
  if (!validList(list)) { fail(404, "Playlist nicht gefunden."); return; }
  std::vector<Track> tracks; String error;
  const bool complete = storage::loadTracks(list, tracks, error);
  beginPage(list.isEmpty() ? "Bewegungsmelder-Musik" : "Playlist: " + list);
  if (!complete) server.sendContent("<p class='warning'>" + html(error) + "</p>");
  if (!list.isEmpty()) {
    const Playlist& p = app::playlists[app::playlistIndex(list)];
    String cfgForm = "<section><h2>Playlist-Einstellungen</h2>" + formStart("/config", list);
    cfgForm += "<label><input type='checkbox' name='shuffle' value='1'" + String(p.shuffle ? " checked" : "") + "> Shuffle</label>";
    cfgForm += "<label><input type='checkbox' name='repeat' value='1'" + String(p.repeat ? " checked" : "") + "> Endlosschleife</label>";
    cfgForm += "<label><input type='checkbox' name='hidden' value='1'" + String(p.hidden ? " checked" : "") + "> In lokaler Auswahl verstecken</label>";
    cfgForm += "<button>Speichern</button></form>" + formStart("/play", list) + "<button>Playlist abspielen</button></form></section>";
    server.sendContent(cfgForm);
  }
  server.sendContent("<section><h2>Titel</h2><p class='muted'>" + String(tracks.size()) + " Titel. Dateiaenderungen stoppen die SD-Wiedergabe.</p><div class='scroll'><table><tr><th>Dateiname</th>");
  if (list.isEmpty()) server.sendContent("<th>Gewicht</th><th>Basisanteil</th>");
  else server.sendContent("<th>Wiedergabe</th>");
  server.sendContent("<th>Entfernen</th></tr>");
  double total = 0; for (const Track& t : tracks) total += t.weight;
  for (const Track& t : tracks) {
    String row = "<tr><td>" + html(t.file) + "</td>";
    if (list.isEmpty()) {
      row += "<td>" + formStart("/weight") + hiddenField("file", t.file) +
             "<input name='weight' type='number' min='0' max='1000' step='0.0001' required value='" + String(t.weight, 4) + "'><button>Speichern</button></form></td><td>" +
             String(total > 0 ? t.weight*100.0/total : 0.0, 1) + " %</td>";
    } else {
      row += "<td>"+formStart("/track",list)+hiddenField("file",t.file)+
             "<button name='action' value='now'>Jetzt abspielen</button> "+
             "<button name='action' value='next'>Als Naechstes</button></form></td>";
    }
    row += "<td><form class='delete' method='post' action='/delete'>" + hiddenField("token", csrf) +
           hiddenField("list", list) + hiddenField("file", t.file) + "<button class='danger'>Loeschen</button></form></td></tr>";
    server.sendContent(row); delay(1);
  }
  server.sendContent("</table></div>");
  if (list.isEmpty()) server.sendContent("<p class='muted'>Anteile gelten vor Ausschluss des vorherigen Titels. Das Wiederholungsverbot veraendert die langfristige Verteilung; bei zwei aktivierten Titeln wird abwechselnd gespielt.</p>");
  else server.sendContent("<p class='muted'>Jetzt abspielen startet ggf. diese Playlist. Als Naechstes gilt fuer die laufende Playlist und ersetzt eine vorherige Vormerkung. Noch ausstehende Titel werden vorgezogen; bereits gespielte koennen erneut gewuenscht werden.</p>");
  server.sendContent("</section><section><h2>MP3 hochladen</h2><form id='uploadForm' method='post' enctype='multipart/form-data' action='/upload?token=" + url(csrf) + "&amp;list=" + url(list) + "'><input id='mp3file' type='file' name='file' accept='.mp3,audio/mpeg' required> <button id='uploadButton'>Hochladen</button></form><p id='uploadState'></p><p class='muted'>");
  server.sendContent(list.isEmpty() ? "Bewegungsmelder: 1-40 Zeichen A-Z, a-z, 0-9 plus .mp3; keine Leerzeichen/Unterstriche." :
    "Playlist: UTF-8-Dateinamen, Leerzeichen und z.B. , ; &amp; sind erlaubt. Maximal 160 UTF-8-Bytes inklusive .mp3. FAT-unzulaessige Zeichen und Steuerzeichen sind gesperrt.");
  server.sendContent(" Maximal 256 MiB pro Datei und 512 Titel pro Ordner. Bestehende Dateien werden nicht ueberschrieben.</p></section>");
  endPage();
}
static void status() {
  String s; s.reserve(600);
  s = "{\"mode\":" + json(app::modeName()) + ",\"pir\":" + (digitalRead(cfg::PIR) == HIGH ? "true" : "false");
  s += ",\"playing\":"; s += audio::state() == audio::State::Running ? "true" : "false";
  s += ",\"paused\":"; s += audio::pauseRequested() ? "true" : "false";
  s += ",\"song\":" + json(app::currentSong) + ",\"playlist\":" + json(app::activePlaylist);
  s += ",\"next\":" + json(app::queuedTrackName());
  s += ",\"sd\":"; s += storage::ready ? "true" : "false";
  s += ",\"hidden\":"; s += apHidden ? "true" : "false";
  s += ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"minHeap\":" + String(ESP.getMinFreeHeap());
  s += ",\"note\":" + json(app::note) + "}";
  headers(); server.send(200, "application/json; charset=utf-8", s);
}
static void handleSettings() {
  if (!authorized()) return;
  uint32_t delay, volume, sleep;
  if (!rules::unsignedNumber(server.arg("delay").c_str(), cfg::MAX_OFF_DELAY_S, delay) ||
      !rules::unsignedNumber(server.arg("volume").c_str(), 100, volume) ||
      !rules::unsignedNumber(server.arg("sleep").c_str(),240,sleep)) { fail(400, "Ungueltige Sekunden/Lautstaerke/Schlafzeit."); return; }
  String error;
  if (!app::settings(delay, static_cast<uint8_t>(volume), server.arg("btname"),sleep, error)) { fail(400, error); return; }
  app::note = "Einstellungen gespeichert."; redirect();
}
static void handleConfig() {
  if (!authorized()) return;
  String list = server.arg("list");
  if (!validList(list, false)) { fail(404, "Playlist fehlt."); return; }
  Playlist p = app::playlists[app::playlistIndex(list)];
  p.shuffle = server.arg("shuffle") == "1"; p.repeat = server.arg("repeat") == "1"; p.hidden = server.arg("hidden") == "1";
  app::beginEdit(); const bool ok = storage::saveConfig(p); app::endEdit();
  if (!ok) { fail(500, "config.txt konnte nicht gespeichert werden."); return; }
  redirect(pageUrl(list));
}
static void handleWeight() {
  if (!authorized()) return;
  const String file = server.arg("file"); double w;
  if (!rules::motionFile(file.c_str()) || !rules::weight(server.arg("weight").c_str(), w)) { fail(400, "Ungueltiger Titel/Gewicht."); return; }
  app::beginEdit();
  std::vector<Track> tracks; String error;
  if (!storage::loadTracks("", tracks, error)) { app::endEdit(); fail(400, error); return; }
  bool found = false;
  for (Track& t : tracks) if (t.file == file) { t.weight = static_cast<float>(w); found = true; break; }
  const bool saved = found && storage::saveWeights(tracks);
  std::vector<Track>().swap(tracks); app::endEdit();
  if (!saved) { fail(found ? 500 : 404, "Gewicht konnte nicht gespeichert werden."); return; }
  redirect("/motion");
}
static void handleDelete() {
  if (!authorized()) return;
  const String list = server.arg("list"), file = server.arg("file");
  if (!validList(list) || !(list.isEmpty() ? rules::motionFile(file.c_str()) : rules::musicFile(file.c_str()))) { fail(400, "Ungueltiger Dateipfad."); return; }
  app::beginEdit(); bool ok;
  { storage::Guard lock; ok = storage::ready && SD.remove(storage::directory(list) + "/" + file); }
  bool csvOk = true;
  if (ok && list.isEmpty()) {
    std::vector<Track> tracks; String error;
    csvOk = storage::loadTracks("", tracks, error) && storage::saveWeights(tracks);
  }
  app::endEdit();
  if (!ok) { storage::healthy(); fail(500, "Datei konnte nicht geloescht werden."); return; }
  issues::clear(storage::directory(list)+"/"+file);
  if (!csvOk) { fail(500, "MP3 geloescht, aber warsch.csv konnte nicht aktualisiert werden."); return; }
  redirect(pageUrl(list));
}
struct UploadContext {
  File file;
  bool active = false, ended = false, editing = false;
  unsigned files = 0;
  uint32_t bytes = 0;
  String error, list, temporary, finalPath;
};
static UploadContext upload;
static void cancelUpload(const String& error) {
  storage::Guard lock;
  if (upload.file) upload.file.close();
  if (!upload.temporary.isEmpty() && storage::ready) SD.remove(upload.temporary);
  if (upload.error.isEmpty()) upload.error = error;
  upload.ended = false;
}
static void finishEditing() {
  if (upload.editing) { upload.editing = false; app::endEdit(); }
  upload.active = false;
}
static void uploadCallback() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    if (upload.active) { ++upload.files; cancelUpload("Nur eine Datei pro Upload erlaubt."); return; }
    upload = UploadContext(); upload.active = true; upload.files = 1;
    if (server.arg("token") != csrf || app::busy() || controls::fullyLocked()) { upload.error = "Sitzung ungueltig oder Geraet gesperrt/beschaeftigt."; return; }
    upload.list = server.arg("list");
    if (!validList(upload.list)) { upload.error = "Playlist nicht gefunden."; return; }
    app::beginEdit(); upload.editing = true;
    if (!storage::ready) { upload.error = "Keine SD-Karte."; return; }
    if (!(upload.list.isEmpty() ? rules::motionFile(u.filename.c_str()) : rules::musicFile(u.filename.c_str()))) {
      upload.error = "Dateiname ungueltig; Namensregeln in README beachten."; return;
    }
    const String dir = storage::directory(upload.list);
    upload.finalPath = dir + "/" + u.filename;
    upload.temporary = dir + "/" + cfg::UPLOAD_TEMP_NAME;
    if (upload.finalPath.length() > cfg::MAX_PATH_BYTES) { upload.error = "Dateipfad zu lang."; return; }
    {
      std::vector<Track> tracks; String error;
      if (!storage::loadTracks(upload.list, tracks, error)) { upload.error = error; return; }
      if (tracks.size() >= cfg::MAX_TRACKS) { upload.error = "Maximal 512 Titel pro Ordner."; return; }
    }
    storage::Guard lock;
    if (SD.exists(upload.finalPath)) { upload.error = "Dateiname existiert bereits; kein Ueberschreiben."; return; }
    SD.remove(upload.temporary);
    upload.file = SD.open(upload.temporary, FILE_WRITE);
    if (!upload.file) { upload.error = "Temporaere Datei konnte nicht angelegt werden."; storage::fault(upload.error); }
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (!upload.active || !upload.error.isEmpty()) return;
    if (u.currentSize > cfg::MAX_FILE_BYTES-upload.bytes) { cancelUpload("Datei ist groesser als 256 MiB."); return; }
    storage::Guard lock;
    if (!upload.file || upload.file.write(u.buf, u.currentSize) != u.currentSize) { cancelUpload("SD-Schreibfehler / Karte voll."); storage::fault(upload.error); return; }
    upload.bytes += u.currentSize;
  } else if (u.status == UPLOAD_FILE_END) {
    storage::Guard lock;
    if (upload.file) { upload.file.flush(); upload.file.close(); }
    upload.ended = upload.error.isEmpty();
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    cancelUpload("Upload abgebrochen."); finishEditing();
  }
}
static void uploadComplete() {
  // Auch wenn kein Multipart-Teil kam, darf kein vorheriger Erfolg wiederverwendet werden.
  if (!upload.active || !upload.ended || upload.files != 1 || !upload.error.isEmpty()) {
    String reason = upload.error.isEmpty() ? "Kein vollstaendiger Upload." : upload.error;
    cancelUpload(reason); finishEditing(); upload = UploadContext(); fail(400, reason); return;
  }
  String error;
  if (!storage::checkMp3(upload.temporary, error)) {
    cancelUpload(error); finishEditing(); upload = UploadContext(); fail(400, error); return;
  }
  bool renamed;
  { storage::Guard lock; renamed = !SD.exists(upload.finalPath) && SD.rename(upload.temporary, upload.finalPath); }
  if (!renamed) {
    cancelUpload("Endgueltiger Dateiname konnte nicht angelegt werden.");
    error = upload.error; finishEditing(); upload = UploadContext(); fail(500, error); return;
  }
  bool csvOk = true;
  if (upload.list.isEmpty()) {
    std::vector<Track> tracks;
    csvOk = storage::loadTracks("", tracks, error) && storage::saveWeights(tracks);
  }
  Serial.printf("[Upload] %s (%lu Bytes)\n", upload.finalPath.c_str(), static_cast<unsigned long>(upload.bytes));
  finishEditing(); upload = UploadContext();
  if (!csvOk) { fail(500, "MP3 gespeichert; Aktualisierung von warsch.csv fehlgeschlagen."); return; }
  headers(); server.send(200, "text/plain; charset=utf-8", "MP3 gespeichert.");
}
// Eigener kleiner Task: 60-s-Sichtbarkeit wird auch bei langen Uploads beendet.
static void hideIfDue() {
  const uint32_t now = millis();
  if (!apHidden.load() && now-apStarted >= cfg::AP_VISIBLE_MS &&
      (lastHideTry == 0 || now-lastHideTry >= 5000)) {
    lastHideTry = now;
    wifi_config_t config = {};
    if (esp_wifi_get_config(WIFI_IF_AP, &config) == ESP_OK) {
      config.ap.ssid_hidden = 1;
      if (esp_wifi_set_config(WIFI_IF_AP, &config) == ESP_OK) {
        apHidden.store(true); Serial.println("[WLAN] SSID verborgen, AP bleibt aktiv.");
      }
    }
  }
}
static void hideTask(void*) {
  while (running.load() && !apHidden.load()) {
    hideIfDue(); vTaskDelay(pdMS_TO_TICKS(500));
  }
  vTaskDelete(nullptr);
}
bool begin() {
  WiFi.persistent(false); WiFi.mode(WIFI_AP);
  const bool startHidden=esp_reset_reason()!=ESP_RST_POWERON;
  const IPAddress ip(cfg::AP_IP[0], cfg::AP_IP[1], cfg::AP_IP[2], cfg::AP_IP[3]);
  if (!WiFi.softAPConfig(ip, ip, IPAddress(255,255,255,0)) || !WiFi.softAP(cfg::AP_SSID, cfg::AP_PASSWORD, cfg::AP_CHANNEL, startHidden, 2)) return false;
  apStarted = millis(); apHidden = startHidden; lastHideTry = 0;
  const char* collected[]={"X-Requested-With","Authorization"}; server.collectHeaders(collected,2);
  char token[17]; snprintf(token, sizeof(token), "%08lx%08lx", static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
  csrf = token;
  server.on("/", HTTP_GET, home);
  server.on("/status", HTTP_GET, status);
  server.on("/bluetooth",HTTP_GET,bluetoothPage);
  server.on("/errors",HTTP_GET,errorsPage);
  server.on("/bt-delete",HTTP_POST,[](){
    if(!admin() || !authorized()) return;
    if(controls::modeLocked()) { fail(423,"Bluetooth-Verwaltung waehrend Modussperre gesperrt."); return; }
    String message; if(!btdevices::remove(server.arg("mac"),message)) { fail(400,message); return; }
    app::note=message; redirect("/bluetooth");
  });
  server.on("/bt-delete-all",HTTP_POST,[](){
    if(!admin() || !authorized()) return;
    if(controls::modeLocked() || server.arg("confirm")!="yes") { fail(400,"Bestaetigung fehlt oder Modus gesperrt."); return; }
    String message; if(!btdevices::removeAll(message)) { fail(500,message); return; }
    app::note=message; redirect("/bluetooth");
  });
  server.on("/error-retry",HTTP_POST,[](){
    if(!authorized()) return;
    app::beginEdit(); String message; const bool ok=issues::retry(server.arg("path"),message);
    issues::flush(true); app::endEdit(); app::note=message;
    if(!ok) { fail(400,message); return; } redirect("/errors");
  });
  server.on("/error-clear",HTTP_POST,[](){
    if(!authorized()) return;
    const String path=server.arg("path");
    bool found=false; for(const auto& e:issues::list()) if(e.path==path) found=true;
    if(!found && !(path.isEmpty() && server.arg("all")=="yes")) { fail(404,"Fehlereintrag nicht vorhanden."); return; }
    app::beginEdit(); issues::clear(path); issues::flush(true); app::endEdit();
    app::note="Fehlerstatus zurueckgesetzt."; redirect("/errors");
  });
  server.on("/motion", HTTP_GET, []() { libraryPage(""); });
  server.on("/playlist", HTTP_GET, []() { libraryPage(server.arg("list")); });
  server.on("/settings", HTTP_POST, handleSettings);
  server.on("/volume",HTTP_POST,[](){
    if(!authorized()) return; uint32_t volume;
    if(!rules::unsignedNumber(server.arg("volume").c_str(),100,volume)) { fail(400,"Ungueltige Lautstaerke."); return; }
    app::setVolume(volume,server.arg("persist")=="1"); headers(); server.send(200,"application/json","{\"ok\":true}");
  });
  server.on("/control",HTTP_POST,[](){
    if(!authorized()) return;
    String error; if(!app::control(server.arg("action"),error)) { fail(400,error); return; } redirect();
  });
  server.on("/track",HTTP_POST,[](){
    if(!authorized()) return;
    const String list=server.arg("list"), file=server.arg("file"), action=server.arg("action");
    if(!validList(list,false) || !rules::musicFile(file.c_str()) || (action!="now" && action!="next")) {
      fail(400,"Ungueltige Playlist, Datei oder Aktion."); return;
    }
    String error;
    if(!app::selectTrack(list,file,action=="next",error)) { fail(400,error); return; }
    redirect(pageUrl(list));
  });
  server.on("/config", HTTP_POST, handleConfig);
  server.on("/weight", HTTP_POST, handleWeight);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/upload", HTTP_POST, uploadComplete, uploadCallback);
  server.on("/play", HTTP_POST, []() {
    if (!authorized()) return;
    String error;
    if (!app::startPlaylist(server.arg("list"), error)) { fail(400, error); return; }
    redirect();
  });
  server.on("/stop", HTTP_POST, []() {
    if (!authorized()) return;
    app::requestStop();
    redirect();
  });
  server.on("/mode", HTTP_POST, []() {
    if (!authorized()) return;
    uint32_t mode;
    if(controls::modeLocked()) { fail(423,"Modus gesperrt."); return; }
    if (!rules::unsignedNumber(server.arg("mode").c_str(), 2, mode)) { fail(400, "Ungueltiger Modus."); return; }
    app::requestMode(static_cast<app::Mode>(mode));
    if (mode == 2) { headers(); server.send(200, "text/html; charset=utf-8", "<meta charset='utf-8'><h1>Wechsel zu Bluetooth</h1><p>WLAN wird ausgeschaltet. Am ESP PLAY druecken und am Smartphone koppeln. Rueckweg: MODE-Taster.</p>"); }
    else redirect();
  });
  server.on("/create", HTTP_POST, []() {
    if (!authorized()) return;
    if (app::playlists.size() >= cfg::MAX_PLAYLISTS) { fail(400, "Maximal 32 Playlists."); return; }
    String error, name = server.arg("name");
    app::beginEdit(); bool ok = storage::createPlaylist(name, error); app::endEdit();
    if (!ok) { fail(400, error); return; } redirect(pageUrl(name));
  });
  server.on("/rescan", HTTP_POST, []() {
    if (!authorized()) return;
    app::note = ""; app::beginEdit(); app::endEdit(); redirect();
  });
  server.on("/unicode", HTTP_POST, []() {
    if (!authorized()) return;
    app::beginEdit(); String report; storage::unicodeTest(report); app::endEdit(); app::note = report; redirect();
  });
  server.onNotFound([]() { fail(404, "Nicht gefunden."); });
  server.begin(); running = true;
  hideTaskStarted = startHidden || xTaskCreatePinnedToCore(hideTask, "hide-ssid", 3072, nullptr, 1, nullptr, 0) == pdPASS;
  if (!hideTaskStarted) Serial.println("[WLAN] SSID-Timer ohne Task; bei HTTP-Auslastung ggf. verzoegert.");
  Serial.printf("[WLAN] %s | %s | SSID %s\n", cfg::AP_SSID, cfg::AP_URL,startHidden?"sofort verborgen":"60 s sichtbar (Power-On)");
  return true;
}
void tick() {
  if (!running.load()) return;
  server.handleClient();
  if (!hideTaskStarted) hideIfDue();
}
void end() { bool wasRunning = running.exchange(false); if (wasRunning) server.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF); }
bool hidden() { return apHidden.load(); }
}

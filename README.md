# Audio-ESP32 V2

PlatformIO-Projekt fuer **NodeMCU-32S / klassischen ESP32 ohne PSRAM**, SPI-microSD, PCM5102A, einen gebuendelten PIR-Eingang, vier Taster mit internen Pull-ups und ein I2C-LCD 16x2.

**Vor dem Flashen: [Pinout](docs/PINOUT.md) lesen. SD-CS wurde von GPIO5 auf GPIO4 verlegt. Die bisherigen PIR-Leitungen an GPIO33 und GPIO21 muessen entfernt werden.**

## Pruefstatus

Die portable Programmlogik wird mit Host-Tests geprueft: UTF-8, Namensregeln, HTML/JSON/URL-Escaping, Zahlen, gewichtete Auswahl, Shuffle, numerische Sortierung sowie Tastenentprellung, Kombinationen, Sperren, Zeitueberlauf und Fades. Aktuelle Ergebnisse und Grenzen stehen im [Testplan](docs/TESTPLAN.md). PlatformIO bleibt auf `espressif32@6.12.0`, aktuell `board = esp32dev`; der Environment-Name `nodemcu-32s` ist nur eine Bezeichnung.

Ein Test auf echter Hardware bleibt erforderlich: insbesondere Verdrahtung, LCD-Scrollen, PLAY-Pause, Skip, SD-/Unicode-Dateinamen sowie Audio- und Bluetooth-Dauerlauf. [Testplan](docs/TESTPLAN.md) vor unbeaufsichtigtem Einsatz abarbeiten.

## 1. Projekt oeffnen und installieren

Den gesamten Ordner mit `platformio.ini` in VS Code/PlatformIO oeffnen; nicht nur `src/main.cpp` kopieren. PlatformIO **Build**, danach **Upload**, Monitor mit 115200 Baud. Alle bisherigen Einstellungen und Dateien sichern. Keine zweite `main.cpp` aus V1 im selben `src` liegen lassen.

Festgelegte Abhaengigkeiten:

- `espressif32@6.12.0`, Arduino-ESP32 2.0.17.
- `ESP8266Audio@1.9.9` fuer MP3-Dateien.
- `ESP32-A2DP` Tag `v1.8.3` fuer Bluetooth; automatischer Download als ZIP, kein manueller Git-Checkout notwendig.

Keine AudioTools-, ArduinoJson- oder LCD-Library erforderlich. Den Arduino-Core oder die Audiobibliotheken nicht einzeln auf aktuelle Hauptversionen umstellen: diese Firmware nutzt bewusst die Legacy-I2S-API des Arduino-ESP32-2.x-Cores.

`partitions.csv` reserviert 3 MiB fuer die Anwendung. Es gibt **kein OTA-Update** und kein internes Musik-Dateisystem. Die NVS-Position bleibt zur V1-Standardpartition passend, sodass die bisherige Nachlaufzeit normalerweise erhalten bleibt. Kein `Erase Flash` fuer ein normales Update notwendig; ein vollstaendiges Loeschen entfernt Einstellungen und Bluetooth-Kopplungen.

Falls wieder `ModuleNotFoundError: No module named 'intelhex'` auftritt, im Windows-PowerShell-Terminal die Python-Umgebung von PlatformIO verwenden:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m pip install intelhex
```

Dieser Standardpfad gilt fuer eine normale PlatformIO-Installation. Ein fehlendes `pio` im normalen PowerShell-PATH bedeutet nicht, dass PlatformIO in VS Code fehlt. CMake Tools ist fuer dieses Arduino/PlatformIO-Projekt nicht erforderlich.

## 2. Konfiguration und WLAN

Hardware, Netzparameter und Limits stehen zentral in **`include/Config.h`**:

| Parameter | Standard |
|---|---|
| WLAN-SSID | `Audio-esp32` |
| WLAN-Passwort | `Musik123` |
| Webadresse | `http://192.168.4.1/` |
| AP-Kanal / gleichzeitige WLAN-Clients | 1 / maximal 2 |
| Sichtbare SSID | 60 Sekunden nur nach `ESP_RST_POWERON`; sonst sofort versteckt |
| Bluetooth-Name | `Audio-ESP32-BT` |
| Pairing-Sichtbarkeitsfenster | 120 Sekunden nach PLAY |
| PIR-Zusatznachlauf | 0 Sekunden, einstellbar 0 bis 600 |
| Gemeinsame SD-Lautstaerke | 55 %, einstellbar 0 bis 100; quadratische Kennlinie |
| Automatischer Deep Sleep | 0 Minuten (aus), einstellbar 0 bis 240 |
| Unterbrechungs-Fade | 500 ms |

Nur nach erkanntem Power-On bleibt die SSID fuer 60 Sekunden sichtbar. Nach Software-Neustart und Deep-Sleep-Wakeup startet der AP direkt versteckt; Bluetooth startet weiterhin gar kein WLAN. AP und Webserver bleiben bei versteckter SSID erreichbar. Die Einordnung richtet sich ausschliesslich nach `esp_reset_reason()`, nicht nach NVS. Ein eigener kleiner Task verbirgt die SSID auch waehrend Uploads; ohne Task erfolgt dies in der Hauptschleife. Verstecken ist kein Zugriffsschutz. Browser `http`, nicht `https`, verwenden; gegebenenfalls im WLAN ohne Internet bleiben.

Die Bluetooth-Geraeteverwaltung hat eine separate HTTP-Digest-Anmeldung: Benutzer `admin`, Passwort `123-Musik`. Beide Werte stehen in `include/Config.h`; ein bei einer aelteren Version in NVS gespeichertes Admin-Passwort wird nicht mehr verwendet. Alle anderen bisherigen Verwaltungsseiten bleiben ueber WLAN erreichbar. Ein sitzungsbezogenes Formular-Token schuetzt schreibende Aktionen vor einfachen fremden Formularaufrufen. Nach Neustart alte Seiten neu laden. Das feste Admin-Passwort kann in `Config.h` vor dem Flashen geaendert werden. HTTP-Digest ersetzt keine HTTPS-Verschluesselung des Seiteninhalts.

## 3. SD-Karte und Migration von V1

FAT32 verwenden; exFAT wird nicht vorausgesetzt. Karte bevorzugt nur bei ausgeschaltetem ESP entnehmen. Ein Sektorlesetest alle 5 s sowie Dateizugriffspruefungen erkennen SD-Ausfaelle. Wiedergabe und Datei-Handles werden geschlossen; alle 10 s wird ohne aktiven Upload ein Remount versucht. Nach Wiederherstellung werden Verzeichnisse neu gelesen, eine Playlist bleibt gestoppt. Die Bewegungsautomatik darf wieder auf PIR reagieren. Unterbrochene FAT-Schreibzugriffe koennen trotz Temp/Backup Schaden verursachen; produktive Karten nicht absichtlich waehrend Schreiben entfernen.

```text
SD-Karte/
  Bewegungsmelder/
    Lied1.mp3
    Lied2.mp3
    warsch.csv
  Playlists/
    Abendmusik/
      config.txt
      01 - Intro.mp3
      Schöne Grüße, Teil 2.mp3
      シンフォニー.mp3
      色.mp3
    Rock/
      config.txt
      Live; Teil 1.mp3
```

Die alten Bewegungsmelder-MP3s **zusammen mit `warsch.csv` aus dem Root nach `/Bewegungsmelder` verschieben**. Der Inhalt der CSV bleibt unveraendert. Es gibt keine automatische Verschiebung oder Loeschung alter Root-Dateien. Fehlende Hauptordner legt die Firmware an. `sd_example/` liefert nur Beispiele fuer Konfigurationsdateien, keine Musik.

Playlistordner werden nur direkt unter `/Playlists` gesucht; verschachtelte Musik-Unterordner werden nicht rekursiv eingelesen. Unter jedem Playlistordner liegt optional:

```ini
shuffle=0
repeat=0
hidden=0
```

Datei: `config.txt`, maximal 512 Bytes. `1` bedeutet aktiv, `0` inaktiv. Fehlt die Datei, sind alle drei Optionen deaktiviert. Ungueltige Felder werden gemeldet; gueltige Felder und Standardwerte werden verwendet. Die Weboberflaeche schreibt diese Datei selbst. `hidden=1` blendet nur aus der lokalen Tasterauswahl aus; Webzugriff und Webstart bleiben erlaubt.

Dateien unter `/Bewegungsmelder/warsch.csv`:

```csv
Lied1,0.5
Lied2,1.5
```

Keine Kopfzeile, Titel ohne `.mp3`, Komma als Feldtrenner und **Dezimalpunkt**. Gewichte 0 bis 1000; 0 deaktiviert. Fehlender Eintrag bekommt 1. Ungueltige Zeilen werden ignoriert; doppelte gueltige Eintraege verwenden den letzten gelesenen Wert. Die Liste darf maximal 16 KiB gross sein. Beim Speichern wird auf vier Nachkommastellen gerundet.

## 4. Betriebsmodi und Taster

**MODE kurz:** Bewegungsmelder -> Playlist -> Bluetooth -> Bewegungsmelder. **MODE 2 s:** funktional ausschalten (Deep Sleep), erst nach Loslassen aller Tasten. Erneutes MODE weckt auf; zuletzt aktiver Modus bleibt erhalten, Playlists starten nicht automatisch. Es gibt keine Status-LEDs; der Modus steht in LCD-Zeile eins. Taster nach GND mit internen Pull-ups: MODE GPIO33, UP/Vorskip GPIO17, DOWN/Backskip GPIO16, PLAY GPIO14.

| Modus | Zweite LCD-Zeile | UP/DOWN | PLAY |
|---|---|---|---|
| Bewegungsmelder | Leer | Keine Funktion | Keine Funktion |
| Playlist, Auswahl | Ausgewaehlter Playlistname; Namen ueber 16 Zeichen scrollen | UP: naechste; DOWN: vorherige sichtbare Playlist | Kurz: Auswahl starten |
| Playlist, aktiv | Aktueller Titel; Titel ueber 16 Zeichen scrollen | UP: naechster Titel; DOWN: bis zu fuenf zuletzt gestartete Titel zurueck | Kurz: pausieren/fortsetzen; 1,5 s halten: Playlist beenden und zur Auswahl zurueckkehren |
| Bluetooth | Name des verbundenen Geraets, ggf. scrollend; waehrend Pairing auch `Pairing ...` | UP: naechster Titel; DOWN: vorheriger Titel per AVRCP | Kurz: bei Verbindung Play/Pause, ohne Verbindung Pairing oeffnen; 3 s halten: Pairing oeffnen |

Das Scrollen aktualisiert nur das LCD zeitgesteuert und blockiert den separaten I2S-Audiotask nicht. Nicht darstellbare Unicode-Zeichen werden auf dem verwendeten Standard-LCD als `?` angezeigt. Eine versteckte Playlist bleibt im Web startbar, wird aber nicht mit den lokalen UP/DOWN-Tasten angeboten.

Das LCD-Backlight startet nach Boot und jedem Moduswechsel fuer 120 s aktiviert. Bei laufender, nicht pausierter Playlist bleibt es immer aktiv. Jeder erkannte Tastendruck schaltet es fuer 120 s ein, auch wenn die Taste im aktuellen Modus keine Aktion ausloest. Nach dem normalen automatischen Ende einer Playlist gilt derselbe 120-s-Zeitraum; ausserhalb davon bleibt das Backlight aus.

**Kindersicherung:** MODE+PLAY gemeinsam 1 s halten schaltet die Modussperre um (kein Moduswechsel und kein manueller Deep Sleep). UP+DOWN gemeinsam 1 s halten schaltet die Gesamtsperre um. Sie hat Vorrang und setzt beim Ein-/Ausschalten die Modussperre zurueck. Bei Gesamtsperre wirkt nur noch UP+DOWN zum Entsperren; Audio/PIR laufen weiter. Jede physische Taste aktiviert weiterhin das Backlight. Sperren gelten in allen Modi, werden 2 s am LCD gemeldet und nach Neustart verworfen. Die gemeinsame Haltezeit beginnt erst mit der zweiten entprellten Taste. Auch abgebrochene Kombinationen loesen keine Einzelaktionen aus; beide Tasten muessen erst losgelassen werden. Einzelaktionen erfolgen beim Loslassen. Der PLAY-Langdruck betraegt bei aktiver Playlist 1,5 s und sonst weiterhin 3 s; die Schwelle wird beim Druecken festgelegt. MODE-Langdruck betraegt 2 s. Bei Gesamtsperre sind zusaetzlich schreibende Webaktionen gesperrt, bei Modussperre Web-Moduswechsel und Bluetooth-Loeschaktionen.

**Automatischer Deep Sleep:** Webeinstellung 0-240 Minuten, 0 deaktiviert. PIR-HIGH und erster Nachlauf, laufende Playlist-Musik bzw. Bluetooth-PCM mit Nutzsignal gelten als Aktivitaet. Eine pausierte Playlist oder nur bestehende Bluetooth-Verbindung verhindert Schlaf nicht. Bewusste Tasten-/Webaktionen setzen den Timer zurueck, `/status`-Polling nicht. Waehrend Sperren, gehaltenen Tasten, Uploads oder Einstellungen wird nicht automatisch geschlafen. GPIO33 weckt ueber EXT0 LOW mit RTC-Pull-up; nach Wakeup wird der RTC-Pin freigegeben und eine noch gehaltene Taste ignoriert. LCD und Backlight werden abgeschaltet. Deep Sleep schaltet die Versorgung von LCD, DAC, Verstaerker und SD-Modul nicht elektrisch ab.

Die zuletzt gestartete sichtbare Playlist und der Betriebsmodus bleiben in NVS gespeichert. Eine nur durchgeblaetterte Auswahl wird nicht bei jedem Tastendruck in Flash geschrieben. Nach Neustart startet eine Playlist **nicht automatisch**; Bewegungsmelder reagieren dagegen direkt. PIR-Module koennen beim Einschalten eine Einlaufphase mit HIGH-Pegeln haben.

**Neustart nur an der Bluetooth-Grenze:** Zwischen den beiden SD-Modi wird direkt umgeschaltet. Zu/von Bluetooth erfolgt ein kontrollierter Neustart nach Speicherung des Zielmodus. Dies trennt WLAN und Bluetooth sowie die beiden I2S-Treiber eindeutig und gibt deren Speicher frei. Wiedergabepositionen werden dabei nicht gespeichert. Bei Neustart im Bluetooth-Modus bleibt WLAN aus; Rueckkehr ueber MODE.

### Bewegungsmelder

Nur GPIO32 wird ausgewertet. Jeder HIGH-Pegel fordert Musik an und erneuert den zusaetzlichen Nachlauf nach LOW. Danach blendet der Titel 500 ms aus und pausiert. Ab der tatsaechlichen Pause beginnt eine zweite gleich lange Nachlaufzeit: Bewegung darin setzt denselben Titel mit Fade-in fort. Bewegung schon waehrend Fade-out kehrt den Fade ohne Titelsprung um. Nach Ablauf des zweiten Fensters endet der Titel; neue Bewegung waehlt neu. Bei Nachlauf 0 entfaellt das Resume-Fenster. Ein separater Audiotask wertet PIR aus, ohne neuen Softwarefilter. Die eigene HIGH-Haltezeit des PIR-Sensors kommt vor dem Software-Nachlauf hinzu.

Die Auswahl bleibt gewichtet zufaellig ohne unmittelbare Wiederholung bei mindestens zwei aktiven Titeln. Dies garantiert keine langfristigen Basisanteile; bei zwei Titeln wird abwechselnd gespielt. Ein einziger aktiver Titel wiederholt sich. Fehlerhafte Dateien werden im Fehlerregister ausgeschlossen; die Gewichtungs-CSV bleibt unveraendert. Neueinlesen allein setzt diese Fehler nicht zurueck, dazu die Fehlerseite verwenden.

### Playlist

Ohne Shuffle werden Dateinamen alphabetisch sortiert; zusammenhaengende ASCII-Ziffern werden dabei numerisch verglichen. Damit ergibt `1 Lied.mp3`, `2 Lied.mp3`, `10 Lied.mp3` die erwartete Reihenfolge. Bei sonst gleichen Zeichen bleibt die UTF-8-Bytefolge massgeblich. Shuffle verwendet eine zufaellige Permutation: jeder Titel genau einmal pro Durchlauf. Repeat startet anschliessend einen neuen Durchlauf; bei Shuffle wird die direkte Wiederholung an der Durchlaufgrenze vermieden, sofern mindestens zwei Titel existieren. Ohne Repeat endet die Wiedergabe und die beendete Playlist bleibt als lokale Auswahl stehen. Keine Gewichtung, keine CSV, keine gespeicherte manuelle Sortierreihenfolge.

In der Playlist-Detailseite kann jeder Titel **Jetzt abspielen** oder **Als Naechstes** gewaehlt werden. Jetzt abspielen startet bei Bedarf die betreffende Playlist; ein laufender Titel wird wie beim Skip ohne Fade gewechselt. Als Naechstes erfordert, dass diese Playlist bereits aktiv ist (Pause ist erlaubt); eine neue Vormerkung ersetzt die vorherige. Der Wunsch wird beim naechsten natuerlichen Titelwechsel oder Vorskip abgespielt, auch am Ende eines nicht wiederholten Durchlaufs. Ein im aktuellen Durchlauf noch ausstehender Titel wird vorgezogen und spaeter nicht erneut automatisch gespielt; ein bereits gespielter Titel wird als zusaetzlicher Wunsch wiederholt. Die verbleibende Sortier- beziehungsweise Shuffle-Reihenfolge, Repeat und der Backskip-Verlauf bleiben erhalten. Beim Beenden, Wechseln oder Neueinlesen der Playlist wird die Vormerkung verworfen; sie wird nicht dauerhaft gespeichert.

### Bluetooth

WLAN, Webserver, SD-Wiedergabe und PIR-Auswertung sind im BT-Modus ausgeschaltet. Ohne bestehende Verbindung oeffnet ein kurzer PLAY-Druck das Pairing; danach am Smartphone koppeln und Medienaudio aktivieren. Bei bestehender Verbindung steuert kurzer PLAY-Druck die Wiedergabe. PLAY 3 s halten oeffnet das Pairing auch bei Verbindung und trennt diese. Eine aktive Tastensperre verhindert das Pairing wie bisher. Der Empfaenger versucht beim Start eine Verbindung zum zuletzt gespeicherten Geraet. Der Standardname kann im WLAN-Webinterface geaendert werden; wirksam beim naechsten BT-Start. Smartphones koennen alte Namen zwischenspeichern: bei Bedarf entkoppeln und neu koppeln.

Die zweite LCD-Zeile zeigt den Namen des aktuell verbundenen und zugelassenen Smartphones. Lange Namen scrollen zeichenweise durch das 16-Zeichen-Fenster; nicht auf dem LCD darstellbare Unicode-Zeichen erscheinen als `?`. Solange der Name noch nicht vom Smartphone geliefert wurde, wird ein zuvor gespeicherter Name oder die MAC-Adresse angezeigt. Ohne Verbindung bleibt die Zeile leer. Ist gleichzeitig ein Pairingfenster aktiv, werden Pairing-Restzeit und Geraetename gemeinsam scrollend angezeigt.

Lautstaerke erfolgt ueber Smartphone/A2DP-AVRCP; die Bibliothek verarbeitet Absolute Volume, soweit das Smartphone es nutzt. Initialer Empfaengerwert ist 64/127. Das SD-Web-Lautstaerkeprofil gilt nicht fuer Bluetooth. UP/GPIO17 und DOWN/GPIO16 senden AVRCP-Vor-/Zurueck, PLAY sendet Play beziehungsweise Pause. Die Befehle werden nur bei zugelassener Verbindung und aktiver AVRCP-Steuerverbindung gesendet. Tastendruck und -loslassen werden im Abstand von 100 ms ohne blockierende Pause uebermittelt; waehrenddessen wird kein weiterer AVRCP-Tastenbefehl angenommen. Fuer Play/Pause wird der vom Smartphone gemeldete AVRCP-Wiedergabestatus genutzt, ersatzweise der A2DP-Streamzustand; die Nutzsignal-/Stille-Erkennung fuer den automatischen Schlaf wird nicht dafuer verwendet. Je nach Smartphone/App koennen Befehle ignoriert werden, BACK kann den aktuellen Titel neu starten, und ein fehlender oder veralteter Status kann die Umschaltung beeintraechtigen.

Neue Kopplungen werden nur im expliziten 120-s-PLAY-Fenster bestaetigt. Ausserhalb ist der Empfaenger nicht sichtbar und neue GAP-PIN-/SSP-Bestaetigungen werden abgelehnt, auch bei einer bereits bekannten MAC. Vorhandene Bonding-Schluessel bleiben erhalten und erlauben regulaeren Reconnect. Die Adresse wird gegen die echte Bondliste geprueft; A2DP verwendet die Authentifizierung des ESP-IDF-Stacks. Ein reiner MAC-Vergleich gilt nicht als Schluesselnachweis. Die Bibliothek entscheidet Adressfreigaben erst beim A2DP-CONNECTED-Ereignis: unerlaubte A2DP-Verbindungen werden dort getrennt und ihre PCM-Ausgabe gesperrt; ein kurzzeitig aufgebauter Funk-/Profilkontakt ist damit nicht ausgeschlossen. Das Pairingfenster ersetzt keinen manuellen numerischen MITM-Vergleich.

Sobald eine Tastensperre aktiv wird, bleibt nur das aktuell verbundene Geraet freigegeben, ersatzweise das letzte bestaetigte Musikgeraet; ohne bekannte Adresse niemand. Diese Freigabe lebt nur im RAM. Bestehende Wiedergabe laeuft weiter, Pairing bleibt geschlossen. Beim Entsperren gelten wieder die Bonding-/Fensterregeln. Als letzte Musikquelle wird erst nach mindestens einer Sekunde empfangener PCM-Samples mit Nutzsignal (Betrag >8 bei 16 Bit, Luecken maximal 300 ms) eine neue MAC gespeichert. Digitale Stille und Lautstaerkeaenderungen zaehlen nicht. Die Hauptschleife schreibt NVS nur bei Aenderung; Audiocallbacks schreiben keinen Flash. Reconnect: hoechstens drei Versuche im Abstand von 10 s. Bibliotheks-Reconnect und deren `last_bda`-NVS werden nicht parallel genutzt; alte Kopplungsschluessel werden uebernommen, die bevorzugte Musikquelle nach dem Update erst durch Musik bestaetigt.

Bluetooth-Geraeteverwaltung: `/bluetooth`, separater Admin-Zugang. Namen (falls bekannt), MAC, Kopplungsstand und bevorzugtes Musikgeraet werden persistent zwischengespeichert, maximal 16 Geraete. Loeschen erfordert eine ausdrueckliche Aktion/Bestaetigung und wird beim naechsten BT-Start ausgefuehrt. Bis die echte Bondliste die Entfernung bestaetigt, bleibt der Auftrag vorgemerkt und das Geraet gesperrt. Anschliessend verschwindet der Eintrag; veraltete Eintraege werden beim Abgleich bereinigt. Alle Kopplungen koennen mit gesonderter Bestaetigung entfernt werden. WLAN und Bluetooth werden auch fuer die Verwaltung nie gleichzeitig gestartet.

Kein aptX/AAC/LDAC oder BLE-Audio zugesichert; verwendet wird A2DP-SBC. Ein WROOM-32U braucht eine angeschlossene externe 2,4-GHz-Antenne; das Boardprofil `esp32dev` aendert daran nichts.

## 5. Webverwaltung

Startseite: Modus, Live-Status, Playlistuebersicht mit direkt aenderbaren Shuffle-/Repeat-/Hidden-Flags, neue Playlist, SD-Lautstaerke, PIR-Nachlauf, Bluetooth-Name, Neueinlesen und Unicode-Diagnose. Jede Playlist hat zusaetzlich eine eigene Seite mit Titeln, Upload und Loeschen. Bewegungsmelder haben ihre eigene Seite mit Gewichtungen.

Ganze Playlistordner lassen sich **nicht per Web loeschen**. Umbenennen von Listen/Titeln ist ebenfalls nicht implementiert; dies bei ausgeschaltetem ESP am PC erledigen. Versteckt ist kein Passwortschutz.

**SD-Verwaltungs-Schreiboperationen stoppen die Wiedergabe**, auch Gewichtungs-/Playlist-Konfigurationsaenderungen. Beim Bewegungsmeldermodus kann Musik danach bei Bewegung wieder beginnen; eine Playlist muss neu gestartet werden. Globale Einstellungen stoppen keine Playlist. Der gemeinsame SD-Regler wirkt bereits beim Bewegen (HTTP gedrosselt), NVS speichert beim Loslassen nur Aenderungen. Pause/Fortsetzen, Skip und Einstellungen arbeiten ohne Seitenneuladen; Upload zeigt Fortschritt. Bluetooth-Lautstaerke bleibt am Smartphone.

SD-Fading: Benutzer-Gain `(Prozent/100)^2` mal Fade-Faktor, hoechstens 1. Manuelles Pause/Stop, Moduswechsel, Deep Sleep und PIR-Nachlauf blenden 500 ms aus; Fortsetzen blendet ein. Neue Titel, natuerliches Ende, automatische Wechsel und manuelles Skip haben keinen Fade. Die veraenderte Kennlinie macht dieselbe Prozentzahl leiser als die alte lineare Amplitude. Keine zusaetzlichen grossen Audiopuffer, kein Crossfade.

Fehlerhafte Dateien: `/errors` zeigt Pfad, Fehlerart, Versuchszahl und Boot-ID mit Sekunden seit Boot (keine RTC-Uhrzeit). Maximal 64 Eintraege in `/.audio-errors.tsv`, gebuendelt mit Temp/Backup auf SD gespeichert, sobald keine aktive Wiedergabe laeuft; auch bei kontrolliertem Abschalten. Bei voller Liste wird die automatische Auswahl vorsorglich ausgesetzt. Einzelne Fehler bzw. alle koennen zurueckgesetzt werden. `Erneut pruefen` prueft Dateizugriff/MP3-Header und gibt den Titel fuer einen Decoder-Versuch frei, keine vorgetaeuschte vollstaendige Integritaetspruefung. Decoderfehler, fehlende Samples, zu lange Synchronisationssuche und vorzeitige Lesefehler werden beim Abspielen erkannt. Eine strukturell gueltige, abgeschnittene MP3 ist nicht immer von einem normalen Ende unterscheidbar. Allgemeine Kartenfehler werden separat gemeldet und fuehren nicht zum Quarantaenisieren aller Titel. Unkontrollierter Stromverlust kann noch nicht gespeicherte Fehlereintraege verlieren.

Upload: eine MP3 pro Anfrage, Streaming in eine temporaere Datei, abschliessende Namensuebernahme, kein Ueberschreiben vorhandener Namen. Headerpruefung auf ID3 oder MP3-Frame-Synchronwort; dies ist **keine vollstaendige MP3-Integritaetspruefung**. Abgebrochene Uploads werden verworfen; liegengebliebene Tempdateien beim Neueinlesen entfernt. Temp/Backup fuer Konfiguration vermindern, aber beseitigen keine Stromausfallrisiken auf FAT. Karte/Versorgung nie waehrend Schreiben entfernen.

Webserver und Haupt-Bedienlogik sind synchron; waehrend eines langen Uploads oder einer stockenden HTTP-Anfrage koennen LCD-Aktualisierungen/Tastendruecke verzoegert sein oder kurze Tastendruecke verloren gehen. Der separate Audiotask hilft beim normalen Lesen, garantiert aber keine unterbrechungsfreie Wiedergabe bei beliebiger SD-/Browserlast. Gleichzeitige grosse Verwaltungsaktionen vermeiden.

## 6. Datei- und Ordnernamen

| Element | Regeln dieser Firmware |
|---|---|
| Bewegungsmelder-MP3 | Titel 1-40 ASCII-Zeichen `A-Z a-z 0-9`, dann `.mp3`; keine Unterstriche, Leerzeichen oder Umlaute |
| Playlistordner | 1-16 ASCII-Zeichen `A-Z a-z 0-9`, Leerzeichen und `_ - ( ) . ,`; keine Umlaute, passend zum 16-Zeichen-LCD |
| Playlist-MP3 | Gueltiges UTF-8, maximal **160 Bytes inkl. `.mp3`** und maximal 128 UTF-16-Codeeinheiten |
| Gesamtpfad | Maximal 224 UTF-8-Bytes |
| Bluetooth-Name | 1-32 UTF-8-Bytes, keine Steuerzeichen oder Rand-Leerzeichen |

Die ASCII-Regeln fuer Bewegungsmelder stammen aus der **tatsaechlich hochgeladenen V1-Datei**; dort waren Unterstriche noch nicht freigeschaltet. Wer sie spaeter ergaenzen moechte, muss `rules::motionFile()` und die Anleitung gemeinsam anpassen.

Playlist-MP3s erlauben z.B. `Schöne Grüße, Teil 1.mp3`, `Musik; live.mp3`, `A & B.mp3`, `50% + Zugabe.mp3`, `シ.mp3`, `色.mp3`. Mehrbyte-Zeichen verbrauchen mehr als ein Byte. `.MP3` wird ebenfalls erkannt.

Nicht erlaubt sind `/ \\ : * ? " < > |`, Steuerzeichen, ungueltige UTF-8-Folgen sowie die ueblichen reservierten ASCII-Geraetenamen `CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`, `LPT1`-`LPT9`. Namen duerfen nicht mit Punkt/Leerzeichen anfangen; der Titel direkt vor `.mp3` darf nicht mit Punkt/Leerzeichen enden. Bei Playlistordnern sind Rand-Leerzeichen und Punkte am Anfang/Ende verboten. Dateien/Ordner mit ungueltigen Namen werden beim Scan ignoriert und im seriellen Monitor gemeldet. Mac-Metadaten wie `._Titel.mp3` werden ignoriert. FAT-Dateinamen sind nicht verlaesslich durch Gross-/Kleinschreibung unterscheidbar.

Die Firmware veraendert Unicode-Namen nicht absichtlich und normalisiert nicht zwischen NFC/NFD. Verzeichnisnamen werden vom Dateisystem uebernommen. Auf dem PC und im Web moeglichst keine nur visuell identischen, unterschiedlich normalisierten Namen parallel erzeugen. Unicode-Darstellung bedeutet nicht, dass jedes Sonderzeichen auf jedem PC/FAT-Treiber als Dateiname zulaessig ist.

Die gepruefte Arduino-ESP32-2.0.17-SDK-Konfiguration setzt `CONFIG_FATFS_LFN_STACK`, `CONFIG_FATFS_MAX_LFN=255` und `CONFIG_FATFS_API_ENCODING_UTF_8`. Die Diagnose zeigt die eingebauten Einstellungen; der Webtest legt eine temporaere UTF-8-Datei mit `ä ß シ 色 , ;` an, listet sie, oeffnet sie erneut, liest sie und loescht sie. Trotzdem **reale MP3s vom Windows-/Mac-Kartenleser testen**, insbesondere zusaetzliche Unicode-Ebenen. Nicht jede Variante ist durch diesen kleinen Test abgedeckt.

## 7. Groessen, RAM und Audio

| Grenze | Wert |
|---|---|
| Playlistordner | Maximal 32; weitere gueltige Ordner werden ignoriert/gemeldet |
| MP3s pro Ordner | Maximal 512 gueltige Dateinamen; bei Ueberschreitung wird die vollstaendige Wiedergabeliste nicht gestartet |
| Einzeldatei | Maximal **256 MiB = 268.435.456 Bytes**, auch bei Kopieren am PC fuer die Wiedergabe geprueft |
| Dateieingangspuffer | 4 KiB fuer die aktuelle MP3 |
| Decoder | Zusaetzlicher libmad-Arbeitsbereich, I2S-DMA, Tasks und Netzwerk benoetigen weiteren RAM |
| Formate | MP3; Mono/Stereo, CBR/VBR; kein WAV/FLAC/AAC-Dateiplayer |

Empfohlene Starttests: MP3 44,1 oder 48 kHz, 128-320 kbit/s. Der Dateiinhalt wird **nicht vollstaendig in RAM geladen**. Im Speicher liegen die aktuelle Titelliste (Dateinamen/Gewichte), kleine Playlistmetadaten, waehrend einer Webansicht gegebenenfalls eine zweite Ordnerliste und die Audiopuffer. Deshalb ist RAM nicht ausschliesslich Musikpuffer. Begrenzte Listen, wiederverwendeter Decoderarbeitsbereich, kurze Datenkopien und Freigabe des Bluetooth-RAM im WLAN-Boot begrenzen den Verbrauch. Kein PSRAM erforderlich eingeplant; verbleibenden Heap im Status/Monitor auf echter Hardware kontrollieren.

Es gibt keine zusaetzliche minutenbasierte Wiedergabebegrenzung. Aus dem 256-MiB-Limit ergeben sich rechnerisch etwa:

| Konstante Bitrate | Maximale Dauer ohne Tag-Overhead |
|---|---|
| 320 kbit/s | 1 h 51 min 51 s |
| 192 kbit/s | 3 h 6 min 25 s |
| 128 kbit/s | 4 h 39 min 37 s |
| 64 kbit/s | 9 h 19 min 14 s |

Bei VBR ist die mittlere Bitrate entscheidend. ID3v2-Cover/Metadaten am Dateianfang werden anhand ihrer Laenge direkt uebersprungen; beschaedigte Tags werden abgewiesen. Fehlerhafte Dateien, seltene MP3-Varianten und langsame SD-Karten koennen dennoch Startprobleme/Aussetzer verursachen. Fremde Daten vor dem ersten ID3-/MP3-Header werden abgewiesen. Kein manuelles Seek, keine Speicherung einer Wiedergabeposition ueber Neustarts, kein Crossfade und keine garantierte lueckenlose Wiedergabe. Pause/Resume innerhalb des laufenden Betriebs wird unterstuetzt. Elektrisches Knacken beim Moduswechsel ist trotz Software-Fade ohne Hardware-Stummschaltung nicht sicher auszuschliessen.

## 8. Quelltextaufteilung

- `src/main.cpp`: Modus, NVS, Wiedergabeplanung, lokale Bedienung.
- `src/AudioEngine.cpp`: MP3, I2S, separater Audiotask, PIR-Nachlauf.
- `src/BluetoothMode.cpp`: A2DP, Pairingfenster, Smartphone-Lautstaerke.
- `src/Storage.cpp`: SD, CSV, Playlist-Konfiguration, Unicode-Test.
- `src/WebUi.cpp`: Webseiten, HTTP, Upload, WLAN/AP.
- `src/Controls.cpp` / `include/ButtonLogic.h`: Tasten, Kombinationen, Sperren und PCF8574-LCD.
- `include/AudioEnvelope.h`: portable Fade- und Lautstaerkekennlinie.
- `src/BtDevices.cpp`: NVS-Geraetecache, bevorzugte Quelle und vorgemerkte Bond-Loeschungen.
- `src/FileIssues.cpp`: begrenztes, persistentes MP3-Fehlerregister.
- `src/Rules.cpp`: portable Validierung, Escaping, Shuffle.
- `include/Config.h`: Pins, Netzwerkdaten, Limits.
- `test/rules_test.cpp` und `test/control_test.cpp`: ohne ESP32 ausfuehrbare Logiktests.

Die Audio-Engine serialisiert alle SD-Zugriffe mit einem rekursiven SD-Mutex; bei gleichzeitigem Bedarf lautet die Sperrreihenfolge Audio -> SD. Zukuenftige Erweiterungen muessen diese Reihenfolge beibehalten.

## 9. Nachweise und weitere Hinweise

Die externe Hardware-/API-Grundlage, nicht als Ersatz fuer einen Boardtest:

- [Arduino-ESP32 2.0.17 SDK-Konfiguration](https://raw.githubusercontent.com/espressif/arduino-esp32/2.0.17/tools/sdk/esp32/dio_qspi/include/sdkconfig.h)
- [Arduino-ESP32 2.0.17 SD/VFS-Implementierung](https://github.com/espressif/arduino-esp32/blob/2.0.17/libraries/FS/src/vfs_api.cpp)
- [ESP8266Audio 1.9.9](https://github.com/earlephilhower/ESP8266Audio/tree/1.9.9)
- [ESP32-A2DP v1.8.3](https://github.com/pschatzmann/ESP32-A2DP/tree/v1.8.3)
- [Espressif GPIO-Beschraenkungen](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/peripherals/gpio.html)
- [PlatformIO Espressif32](https://docs.platformio.org/en/latest/platforms/espressif32.html)

Bibliotheken werden von PlatformIO separat bezogen und unterliegen ihren jeweiligen Lizenzen. Keine Bibliotheksquellen oder Musikdateien sind in diesem Projektarchiv enthalten.

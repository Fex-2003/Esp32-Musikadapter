# Inbetriebnahme und Testplan

## Tatsaechlich durchgefuehrte Softwarepruefungen

| Pruefung | Ergebnis / Aussagegrenze |
|---|---|
| Portable C++11-Tests gegen `Rules.cpp` | Am 28.10.2026 erneut bestanden mit g++/`-Wall -Wextra -Werror`; dieser Lauf ohne Sanitizer |
| Ungueltiges UTF-8, Umlaute, CJK, 4-Byte-Zeichen, Grenzlaengen, verbotene Zeichen | Regeln in Tests geprueft; kein realer FAT-/Browsernachweis |
| Gewichtete Auswahl | 100.000 Ziehungen gegen Basisverteilung; Wiederholungsverbot separat geprueft |
| Shuffle | Permutationen fuer 0-512 Titel, einschliesslich Durchlaufgrenze, geprueft |
| `test/control_test.cpp` | Am 29.10.2026 bestanden: kurze/lange Tasten, Boot mit gedrueckter Taste, versetzte/abgebrochene/ueberlappende Kombinationen (auch zweite Taste unmittelbar vor Langdruckgrenze), Sperrprioritaet, Backlight-Ereignis trotz Sperre, millis-Ueberlauf und Fade-Umkehr |
| `test/playlist_order_test.cpp` | Vorziehen ausstehender Titel, zusaetzlicher Wunsch bereits gespielter Titel, Durchlaufende und Erhalt der verbleibenden Shuffle-Reihenfolge als Hosttest geprueft; keine Hardware-/HTTP-Integration |
| Web-JavaScript | Eingebettetes Script am 28.10.2026 mit JavaScript-Parser (`new Function`) syntaktisch geprueft; kein Browser-/HTTP-Hardwaretest |
| Echte PlatformIO-Abhaengigkeiten / Xtensa-Build / Linken | Erfolgreich: Environment `nodemcu-32s`, `board = esp32dev`, espressif32 6.12.0; statisches RAM 18,9 %, Anwendungs-Flash 59,3 % (kein Laufzeit-Heapnachweis) |
| Flashbelegung, Laufzeit-Heap, Audio/BT/LCD/SD | **Nicht auf echter Hardware getestet** |

Die aktuellen Firmwarepruefungen verwenden die echten installierten ESP32-/Bibliotheksheader, keine Schnittstellen-Stubs. Ein erfolgreicher Build bestaetigt keine reale Funk-, Audio- oder Deep-Sleep-Funktion.

Portable Tests lokal unter Linux/macOS oder einer Umgebung mit g++:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -Iinclude src/Rules.cpp test/rules_test.cpp -o rules_test
./rules_test
g++ -std=c++11 -Wall -Wextra -Werror -Iinclude test/control_test.cpp -o control_test
./control_test
g++ -std=c++11 -Wall -Wextra -Werror -Iinclude test/playlist_order_test.cpp -o playlist_order_test
./playlist_order_test
```

Optional `-fsanitize=address,undefined` ergaenzen, sofern der lokale Compiler dies unterstuetzt. Fuer die echte Firmware PlatformIO Build verwenden; diese Hosttests ersetzen das nicht.

## 1. Vor Spannung anlegen

SD-CS an GPIO4; PIR nur GPIO32. GPIO21/33 nicht mehr mit alten PIR-Ausgaengen verbunden. MODE an GPIO33, UP/Vorskip an GPIO17, DOWN/Backskip an GPIO16 und PLAY an GPIO14 jeweils per Taster nach GND, interne Pull-ups. LCD-5-V-Pull-ups durch Pegelwandler vom ESP trennen. PCM-Jumper, Massebezug, passende Modulversorgung und I2S-Reihenfolge kontrollieren. Beim WROOM-32U externe Antenne anschliessen. Verstaerkerlautstaerke klein.

## 2. Erster Build und Start

Gesamtes Projekt oeffnen, Build ausfuehren, erste echte Fehlermeldung sichern, nicht nur die letzte `FAILED`-Zeile. Bei Erfolg Upload und Monitor115200. Beim Start Version, Modus, SD-Status, UTF-8-Konfiguration, LCD-Adresse und Heap lesen. Kein erwarteter fester Heapwert behauptet; Minimum und Stabilitaet auf dem Board beobachten.

Erster Start ohne vorhandenen V2-Moduseintrag: Bewegungsmelder. Mit vorhandener NVS wird letzter Modus verwendet. Ist das Geraet bereits im Bluetooth-Modus, MODE druecken, um WLAN wieder zu erhalten.

## 3. Grundtest Bewegungsmelder

Zwei normale MP3s nach `/Bewegungsmelder`, passende CSV. GPIO32 testweise mit 3V3 verbinden: Musik beginnt ohne Fade. Nach LOW endet sie bei Nachlauf 0 nach 500 ms Fade. Bei Nachlauf 5: nach 5 s LOW 500 ms ausblenden, dann Pause. HIGH innerhalb weiterer 5 s setzt denselben Titel mit 500 ms Fade-in fort. HIGH waehrend Fade-out kehrt den Fade ohne Sprung um. Nach Ablauf des zweiten Fensters wird beendet; Bewegung danach waehlt neu. Pegel am ESP messen; kein Signal darf 5 V liefern. Zwei aktive Titel wechseln wegen Wiederholungsausschluss.

## 4. Playlist und Unicode

Neue Liste `Test Musik` im Web anlegen. Verschiedene Dateien mit `ae`, Umlauten, `ß`, `シ`, `色`, Komma, Semikolon, `&`, `%`, `+` testen. Web-Unicode-Diagnose starten. Dateien ausserdem per Windows/macOS direkt auf SD kopieren, neu starten und Auflisten, Abspielen sowie Loeschen vergleichen. Ungueltige Namen und zu lange Namen sollen abgewiesen werden. Keine beliebigen Unicode-Zeichen im Playlist-Ordnernamen erwarten: LCD-Regeln gelten dort absichtlich.

Alle vier Kombinationen aus Shuffle/Repeat mit drei kurzen MP3s pruefen. Ohne Shuffle `1 Lied`, `2 Lied`, `10 Lied` verwenden. Kurzer PLAY pausiert/setzt fort, UP/GPIO17 startet den naechsten Titel, DOWN/GPIO16 geht hoechstens fuenf gestartete Titel zurueck. 1,5 s PLAY beendet mit Fade und zeigt wieder die Auswahl. Ohne Repeat am Ende zur Auswahl und 120 s Backlight. Skip und natuerlicher Wechsel ohne Fade. Lange Titel scrollen ohne Audioaussetzer. Hidden: lokal weg, im Web startbar. Neustart/Wakeup: Optionen bleiben, Playlist startet nicht. Leere/komplett fehlerhafte Playlist meldet den Zustand, ohne Endlosschleife.

## 5. Bluetooth

MODE bis Bluetooth: WLAN muss verschwinden. Ohne Verbindung PLAY kurz: LCD-Zeile zwei zeigt die Pairing-Restzeit. Am Smartphone nach konfiguriertem Namen suchen, koppeln, Medienaudio abspielen, Lautstaerke veraendern. Bei Verbindung PLAY kurz pausiert/setzt fort, UP/GPIO17 springt vor, DOWN/GPIO16 zurueck. PLAY 3 s halten trennt die Verbindung und oeffnet ein neues Pairingfenster. Nach 120 s ohne Kopplung nicht mehr in normaler Suche sichtbar. Bereits gekoppeltes Telefon darf weiter reconnecten.

Bluetooth-Abnahme: Play/Pause bei Musik, bei digitaler Stille und beim direkt am Smartphone geaenderten Zustand pruefen. Vor/Zurueck in mindestens zwei Apps testen; ein Neustart des aktuellen Titels bei BACK ist smartphoneabhaengig. Mit Modussperre bleiben Play/Pause und Skip aktiv, Pairing wird verweigert. Mit Gesamtsperre sind alle vier Aktionen gesperrt, UP+DOWN entsperrt weiterhin. Bei fehlender AVRCP-Verbindung oder nicht unterstuetztem Befehl darf kein Absturz erfolgen. 100-ms-Befehlsabstand und Moduswechsel/Pairing waehrend laufender Ausgabe pruefen.

LCD: Mit zwei Smartphones mit unterschiedlichen Namen verbinden; kurze Namen statisch, Namen ueber 16 Zeichen scrollend anzeigen. Nicht darstellbare Unicode-Zeichen erscheinen als Fragezeichen. Beim Trennen verschwindet der Name, beim Reconnect wird vor Namensantwort ggf. der gespeicherte Name oder die MAC gezeigt. Pairingfenster mit aktiver Verbindung muss Pairing-Restzeit und Namen anzeigen; auch bei langen Namen auf Audioaussetzer achten.

MODE zurueck: Neustart in Bewegungsmelder, WLAN wieder erreichbar. Mindestens zehnmal SD -> BT -> SD wechseln und I2S-/Speicherfehler suchen. BT-Name im WLAN aendern und neuen BT-Start pruefen; Smartphone-Cache beachten.

## 6. Web/Upload/Fehlerfaelle

Upload pausiert/stoppt SD-Musik. Lange MP3 hochladen, Browserabbruch testen, danach keine fertige unvollstaendige Datei akzeptieren. Doppelten Namen nicht ueberschreiben. Groesser256MiB ablehnen. `config.txt` speichern und Neustart pruefen. Ohne SD ist Bluetooth weiterhin benutzbar, SD-Modi zeigen Fehler.

Statusseiten waehrend Musik oeffnen; auf Knackser/Aussetzer achten. Bei Problemen SD-Leitungen, Netzteil, Kartentyp, MP3-Encoding und Heap pruefen. Kein Hot-Unplug-Test an produktiven Daten und keine absichtlichen Stromausfaelle waehrend Schreiben ohne gesicherte Testkarte.

## 7. Dauerlauf

Mehrere Stunden Playlist-Repeat, dann wiederholte Moduswechsel. `heap` und `minHeap` aus `/status` bzw. seriellen Monitor verfolgen. Auf wachsenden RAM-Verbrauch, Neustarts/Brownouts, verlorene SD-Verbindung, WLAN-Reconnect und Lautstaerke achten. Audioausgabe getrennt mit Kopfhoerer-Verstaerker/Line-Eingang pruefen; passive Lautsprecher nicht direkt am DAC anschliessen.

## 8. Erweiterungs-Abnahme auf Hardware (noch offen)

Alle folgenden Punkte sind **nicht auf Hardware verifiziert**, sondern ein Abnahmeplan:

1. Power-On in SD-Modus: 60 s sichtbare SSID. Software-Neustart/BT-Rueckkehr/Deep-Sleep-Wakeup: sofort versteckt, manuell erreichbar. Im BT-Modus nie WLAN.
2. MODE kurz wechselt erst nach Loslassen, 2 s fordert Schlaf an. MODE weiter halten: nicht schlafen/wieder aufwachen. Erst loslassen, dann erneuter kurzer Druck weckt. In allen drei Modi testen.
3. Nach Wakeup gleicher Modus, keine automatische Playlist. MODE gehalten lassen: kein weiterer Moduswechsel/Schlaf durch denselben Druck. LCD startet mit 120 s Backlight.
4. MODE+PLAY 1 s: Sperre, kein MODE-/PLAY-Einzelereignis. Zweite Taste unmittelbar vor 2 s MODE bzw. 1,5 s PLAY druecken: kein vorzeitiger Langdruck. Kombination vorzeitig loesen: ebenfalls keine Einzelaktion.
5. UP+DOWN 1 s: Gesamtsperre. Musik/PIR laufen weiter, normale Tasten und schreibende Webaktionen gesperrt. Entsperren entfernt auch Modussperre. Vier Tasten und versetztes Loslassen testen.
6. Sperrmeldungen passen auf 16 Zeichen und verschwinden nach 2 s. Jeder Tastendruck aktiviert das Backlight trotz Sperre bzw. ohne Funktion.
7. PLAY-Pause/Resume mehrfach waehrend eines Fades umkehren. MODE/PLAY-Langdruck kurz vor Dateiende: kein haengender Uebergang. Skip/Natural-End ohne Fade, neue MP3 ohne Startfade.
8. SD-Regler waehrend Musik: sofortige Aenderung ohne Stottern. 0 stumm, 100 nicht uebersteuert. Neustart behaelt Wert. NVS nicht pro Audioframe/Statuspoll schreiben.
9. Unbekanntes Smartphone ausserhalb Pairingfenster muss abgelehnt werden, auch mit zwischengespeichertem Namen/Adresse. Im Fenster koppeln, nach 120 s keine neue Kopplung. Bestehenden Bond auf ESP behalten, nur am Telefon loeschen: erneutes Pairing ausserhalb Fenster ebenfalls ablehnen.
10. A spielt >1 s Musik, B verbindet nur/streamt digitale Stille/aendert Lautstaerke: bevorzugte Quelle bleibt A. B spielt >1 s: B wird bevorzugt und bleibt nach Power-Off/Wakeup gespeichert.
11. Bevorzugtes Telefon ausschalten: maximal drei Reconnect-Versuche, Abstand 10 s; Bedienung/Pairing bleiben nutzbar. Danach anderes berechtigtes Telefon manuell verbinden.
12. Sperre mit verbundenem A: A laeuft weiter und darf nach Verlust reconnecten; bekanntes B und unbekanntes C erhalten keine Audiofreigabe. Ohne Verbindung: bevorzugtes Geraet. Ohne bevorzugtes Geraet: niemand.
13. Entsperren erlaubt wieder normale bekannte Geraete, aber kein neues Pairing ohne PLAY. Sperren/Entsperren darf die bevorzugte NVS-Adresse nicht aendern.
14. `/bluetooth`: Digest-Login mit Benutzer `admin` und Passwort `123-Musik` pruefen, auch nach Update mit altem NVS-Eintrag. Mit falschem Passwort keine Liste/Loeschaktion. Tokenfehler und fehlende Bestaetigung fuer Alle-Loeschen serverseitig ablehnen. Namen und MAC korrekt anzeigen.
15. Einzelnen Bond entfernen: Auftrag im WLAN sichtbar; Power-Off vor BT-Start verliert Auftrag nicht. Beim BT-Start abgleichen/loeschen, Rueckkehr ins Web: Eintrag weg. Bevorzugtes geloeschtes Geraet darf nicht reconnecten. Wiederholung/fehlender Eintrag unkritisch.
16. Alle Kopplungen entfernen, BT starten, Web erneut pruefen. Alte Telefone muessen im Pairingfenster neu gekoppelt werden. Bestehende Bonds ohne Loeschaktion nicht entfernen.
17. Auto-Sleep auf 1 min: in Playlist-Pause/Auswahl und BT ohne Nutzsignal schlafen; Wiedergabe bzw. PIR/Nachlauf verhindert Schlaf. Statuspolling alle 3 s verhindert Schlaf nicht. Tasten/Webaktion erneuert Timer. Sperren/Upload verhindern Schlaf, 0 deaktiviert.
18. Testkarte fehlt beim Start: LCD/Webfehler, BT weiterhin nutzbar. Testkarte im Leerlauf einsetzen: spaetestens nach Remountintervall bereit. Lesefehler mit gesicherter Testkarte: Audio stoppt, nach Wiederherstellung Playlist nicht ungefragt starten.
19. Leere/falsche MP3, defekter ID3-Header, langes Datenrauschen und beschaedigte MP3 testen. Fehlerliste persistent, einzelne Dateien ausgeschlossen; gesunde Titel weiter. Karte entfernen darf nicht alle Dateien als defekt markieren. Header-Recheck versus vollstaendige Decoderpruefung unterscheiden.
20. Smartphone-Layout, Pause/Skip/Regler ohne Reload, Unicode-Uploadfortschritt, Loeschbestaetigung, Config/CSV und Uploadabbruch pruefen. Anschliessend mehrstuendiger Dauerlauf inkl. BT-/SD-Wechsel und Sleep/Wakeup mit Heapbeobachtung.

Bluetooth-Grenze: `set_address_validator()` greift in Version 1.8.3 erst nach A2DP-Verbindungsaufbau; PCM ist bei Ablehnung gesperrt. Es wird keine Garantie gegeben, dass ein unerlaubtes Geraet nicht kurzzeitig einen ACL-/Profilkontakt sieht. GAP-Kopplungsablehnung und bestehende Link-Key-Authentifizierung muessen mit mehreren echten Smartphones getestet werden.

Zusatztest Titelwahl: In der aktiven Playlist bei Shuffle an/aus je einen ausstehenden und einen schon gespielten Titel direkt bzw. als Naechstes waehlen. Vormerkung ersetzen; natuerlichen Wechsel, Vorskip, Pause, Backskip und Ende ohne Repeat pruefen. Restliche Reihenfolge und die hoechstens fuenf zurueckliegenden Titel kontrollieren. Aus einer anderen oder nicht gestarteten Playlist muss Als Naechstes mit einem Hinweis abgelehnt werden; Jetzt abspielen startet diese Playlist. Dateifehler, gesperrte Tasten und Mode-Wechsel waehrend PIR-Musik pruefen. Die Vormerkung darf nach Beenden/Neueinlesen nicht erhalten bleiben.

## 9. Nachweise der verwendeten APIs

- [ESP-IDF 4.4.8: EXT0, RTC-Pull-up und rtc_gpio_deinit](https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32/api-reference/system/sleep_modes.html)
- [ESP-IDF 4.4.8: GAP-Authentifizierung und Bondverwaltung](https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32/api-reference/bluetooth/esp_gap_bt.html)
- [ESP-IDF 4.4.7 A2DP: BTA_SEC_AUTHENTICATE bei ausgehenden und eingehenden Verbindungen](https://github.com/espressif/esp-idf/blob/v4.4.7/components/bt/host/bluedroid/btc/profile/std/a2dp/btc_av.c)
- Installierte ESP32-A2DP 1.8.3: Adressvalidator in `handle_connection_state`, ueberschreibbare GAP-/PCM-Callbacks. Keine Bibliotheksdateien veraendert.

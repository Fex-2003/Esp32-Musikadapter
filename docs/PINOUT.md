# Pinout V2 - NodeMCU-32S

**Alle Nummern sind GPIO-Nummern, keine laufenden Positionen der Stiftleiste.** Ziel ist der klassische ESP32/NodeMCU-32S ohne PSRAM. Nicht unveraendert auf S3/C3 oder ein WROVER-/PSRAM-Board uebertragen. Am konkreten Board die aufgedruckten GPIO-Bezeichnungen verwenden.

## 1. Aenderungen gegenueber der hochgeladenen V1

| Funktion | V1 | V2 | Notwendige Aenderung |
|---|---|---|---|
| SD CS | GPIO5 | **GPIO4** | CS-Leitung umstecken |
| PIR-Signale | GPIO32, GPIO33, GPIO21 | Nur **GPIO32** | Externe Dioden-ODER-Schaltung nur an GPIO32 |
| GPIO33 | PIR 2 | MODE-Taster | Alte PIR-Leitung entfernen |
| GPIO21 | PIR 3 | LCD SDA | Alte PIR-Leitung entfernen |
| Audio | GPIO27/26/25 | Unveraendert | Keine |
| SD Daten/Takt | GPIO23/19/18 | Unveraendert | Keine |

GPIO5 ist ein Strapping-Pin. Die neue Belegung nutzt ihn nicht. Die USB-/UART-Pins GPIO1/3 bleiben frei. GPIO6-11 sind fuer das Flash reserviert; diese nicht verwenden. GPIO16/17 sind hier fuer das klassische Board ohne PSRAM vorgesehen.

## 2. Gesamte GPIO-Belegung

| GPIO | Anschluss | Richtung am ESP | Hinweis |
|---|---|---|---|
| 4 | SD CS | Ausgang | In V1 war es GPIO5 |
| 18 | SD CLK/SCK | Ausgang | SPI-Takt, 10 MHz konfiguriert |
| 19 | SD MISO | Eingang | Daten von der SD |
| 23 | SD MOSI | Ausgang | Daten zur SD |
| 25 | PCM5102A DIN | Ausgang | I2S-Daten; am ESP oft DOUT genannt |
| 26 | PCM5102A LCK/LRCK/WS | Ausgang | I2S-Kanaltakt |
| 27 | PCM5102A BCK/BCLK | Ausgang | I2S-Bittakt |
| 32 | Gebuendelter PIR-Ausgang | Eingang | 3,3-V-Logik, interner Pull-down |
| 33 | MODE-Taster | Eingang | Interner Pull-up; Taster nach GND |
| 17 | UP / Vorskip-Taster | Eingang | Interner Pull-up; Taster nach GND |
| 16 | DOWN / Backskip-Taster | Eingang | Interner Pull-up; Taster nach GND |
| 14 | PLAY/PAUSE/PAIR-Taster | Eingang | Interner Pull-up; Taster nach GND |
| 21 | LCD SDA | Bidirektional | I2C-Daten, ggf. Pegelwandler |
| 22 | LCD SCL | Ausgang/BUS | I2C-Takt, ggf. Pegelwandler |

I2C heisst **SDA/SCL**, nicht SDL/SCA. PCM5102A nutzt dagegen **I2S**. Die Busse sind verschieden.

## 3. Taster

Je ein Schliessertaster von GPIO nach GND. MODE (GPIO33), UP/Vorskip (GPIO17), DOWN/Backskip (GPIO16) und PLAY/PAUSE (GPIO14) verwenden alle `INPUT_PULLUP`; externe Pull-up-Widerstaende sind nicht erforderlich.

```text
GPIO33 / GPIO16 / GPIO17 / GPIO14 ---- Taster ---- GND
```

Die Entprellzeit ist 35 ms. Bei Boot gedrueckte Taster loesen erst nach Loslassen und erneutem Druecken aus. Ein kurzer PLAY-Druck pausiert bzw. setzt eine aktive Playlist fort; ein 1,5-s-Langdruck beendet sie. UP/DOWN haben keine automatische Wiederholung bei langem Halten.

Bluetooth: bei verbundenem Smartphone PLAY kurz fuer Play/Pause, UP fuer naechsten und DOWN fuer vorherigen Titel. Ohne Verbindung oeffnet PLAY kurz das Pairing; PLAY 3 s halten oeffnet es auch bei Verbindung. Bluetooth-Sperren und Smartphone-Unterstuetzung beachten.

MODE 2 s: nach Loslassen Deep Sleep; erneut MODE weckt ueber GPIO33/EXT0 LOW auf. Der RTC-Pull-up ist im Schlaf aktiv. MODE+PLAY 1 s sperrt/entsperrt den Modus, UP+DOWN 1 s alle Tasten. Kombinationen unterdruecken auch bei vorzeitigem Abbruch Einzelaktionen. Es gibt weiterhin keine Modus-LEDs.

## 4. PIR-Eingang mit Dioden-ODER

Die PIR-Module bleiben getrennt versorgt. Alle Signalmassen muessen mit ESP-GND verbunden sein. Die Diode je Ausgang zeigt vom PIR zum gemeinsamen Signal:

```text
PIR1 OUT ----|>|----+
PIR2 OUT ----|>|----+---- GPIO32
PIR3 OUT ----|>|----+
                       |
                   10 kOhm (empfohlen bei laengerer Leitung)
                       |
                      GND
```

Kathoden/Markierungsringe am gemeinsamen GPIO-Knoten, Anoden an den PIR-Ausgaengen. Schottky-Dioden verringern den Spannungsabfall. Der interne Pull-down am GPIO32 ist aktiv; ein externer Widerstand ist nicht erforderlich. Bei Stoerungen Verkabelung, Massebezug, Filterung und gegebenenfalls Schmitt-Trigger-Puffer pruefen.

**Nach der Diode muss noch ein sicherer HIGH-Pegel am ESP ankommen.** Nicht nur die 3,3 V am einzelnen PIR-Ausgang messen. Keine 5-V-/12-V-Signale direkt anschliessen. Die Sensorversorgung ist modulspezifisch und nicht mit ihrer Ausgangsspannung gleichzusetzen. Eine zusammengefasste Signalader ersetzt keinen gemeinsamen Massebezug.

## 5. PCM5102A

| Modulanschluss | Verbindung |
|---|---|
| VIN/VCC | Versorgung gemaess genau dieser Modulplatine; NICHT pauschal 3V3 oder 5V annehmen |
| GND | ESP-GND |
| BCK | GPIO27 |
| LCK/LRCK | GPIO26 |
| DIN | GPIO25 |
| SCK/MCLK | Fuer Drei-Leiter-I2S gemaess Modulschaltung an GND; kein ESP-Takt vorgesehen |
| LOUT/ROUT/AGND | Zum Line-/AUX-Eingang des Verstaerkers oder Klinkenbuchse nutzen |

Typische Konfiguration: FLT LOW, DEMP LOW, XSMT HIGH, FMT LOW fuer I2S. Bei dem bekannten GY-Board entsprechen diese oft H1=L, H2=L, H3=H, H4=L. **Die tatsaechliche Jumper-Zuordnung am Modul pruefen**, nicht ungeprueft uebertragen. Keinen Pin auf 3V3 verdrahten, wenn eine vorhandene Loetbruecke ihn bereits mit GND verbindet.

Das PCM5102A-Modul ist ein Line-Ausgang, kein passiver Lautsprechertreiber. Verstaerkerlautstaerke beim ersten Test klein stellen. Gemeinsame Signalmasse, kurze I2S-Leitungen und saubere Versorgung verwenden. Kein extra GPIO fuer XSMT vorgesehen; daher kann die Firmware elektrische Einschalt-/Umschaltgeraeusche nicht garantiert unterdruecken.

## 6. I2C-LCD 16x2

Unterstuetzt ist ein HD44780-kompatibles 16x2-LCD mit **typischem PCF8574-Backpack**:

| PCF8574-Bit | LCD-Verbindung |
|---|---|
| P0 | RS |
| P1 | RW |
| P2 | E |
| P3 | Backlight, aktiv HIGH |
| P4-P7 | D4-D7 |

Bei `cfg::LCD_ADDRESS = 0` durchsucht die Firmware den I2C-Bereich `0x03` bis `0x77` und verwendet die erste antwortende Adresse. Eine feste Adresse kann weiterhin in `cfg::LCD_ADDRESS` vorgegeben werden. Ein anderer Backpack-Pinplan oder MCP23008 wird nicht automatisch unterstuetzt. Ohne gefundenes LCD laufen Taster und Audio weiter. Kontrastpoti am LCD einstellen; beleuchtete Kaestchen allein beweisen keine funktionierende Initialisierung.

**Wichtig bei den verbreiteten 5-V-Modulen:** Ihre I2C-Pull-ups liegen haeufig an 5 V. ESP32-GPIOs sind nicht 5-V-tolerant. Ein direktes Verbinden ist daher keine sichere Standardloesung.

Empfohlener Aufbau fuer ein 5-V-LCD:

```text
ESP GPIO21 (SDA) -- LV-SDA [bidirektionaler I2C-Level-Shifter] HV-SDA -- LCD SDA
ESP GPIO22 (SCL) -- LV-SCL [bidirektionaler I2C-Level-Shifter] HV-SCL -- LCD SCL
ESP 3V3          -- LV
5V               -- HV und LCD VCC
GND              -- gemeinsame Masse von ESP, Wandler und LCD
```

Einen I2C-tauglichen Open-Drain-Pegelwandler samt passenden Pull-ups verwenden, keinen blossen Spannungsteiler. Alternativ ein **ausdruecklich fuer 3,3-V-Versorgung und 3,3-V-Logik spezifiziertes** LCD-Modul. Einen unbekannten 5-V-PCF8574-Aufbau nur mit auf 3,3 V umgelegten Pull-ups zu betreiben ist nicht allgemein garantiert, da dessen HIGH-Schwellwert von VCC abhaengt.

Die Software nimmt einen einzigen I2C-Bus mit 100 kHz. Keine LCD-Bibliothek manuell installieren.

## 7. SD und Versorgung

SD-Karte und alle ESP-Digitalsignale arbeiten mit 3,3-V-Logik. Bei SD-Breakouts mit Regler/Pegelwandler haengt die benoetigte VCC-Spannung von der Platine ab. Falsche Speisung eines 5-V-Eingangs mit 3,3 V kann nach dem Regler zu Unterspannung fuehren. Umgekehrt darf eine nackte 3,3-V-Karte niemals 5 V erhalten.

Ein stabiles, zum Board passendes USB-/5-V-Netzteil und ausreichende Reserve fuer LCD/SD/ESP vorsehen. Die 3V3-Reglerbelastbarkeit des konkreten Boards beachten. Keine fremden 5-V-Quellen unkontrolliert parallel zu USB/VIN schalten. Zuerst stromlos verdrahten und Versorgung pruefen, dann verbinden.

## Quellen

- [Espressif GPIO-Dokumentation](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/peripherals/gpio.html)
- [ESP32-Datenblatt als HTML](https://documentation.espressif.com/esp32_datasheet_en.html)
- [NXP PCF8574/PCF8574A](https://www.nxp.com/products/interfaces/ic-spi-i3c-interface-devices/general-purpose-i-o-gpio/remote-8-bit-i-o-expander-for-icbus-with-interrupt:PCF8574_74A)
- [Texas Instruments PCM5102A](https://www.ti.com/product/PCM5102A)

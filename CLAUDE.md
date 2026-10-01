# GIGA Dashboard – Projektgedächtnis und Regeln

Schreibtisch-Instrument für Rubens PC: Arduino GIGA R1 WiFi + GIGA Display Shield zeigt Uhrzeit
(analog + digital), Temperaturen und Auslastung von CPU, GPU und RAM und – solange Spotify spielt –
Titel, Interpret und Cover (das Cover zusätzlich unscharf und abgedunkelt als Bildschirmhintergrund,
sonst ist der Hintergrund schwarz). Eine Python-Bridge auf dem PC liefert Zeit, HWiNFO-Werte und den Song über
USB-Seriell; ohne PC läuft die Uhr mit der RTC des GIGA weiter. **Nur Anzeige – kein Touch, keine SignalRGB-Steuerung**
(beides am 30.09.2026 auf Rubens Wunsch entfernt, Gründe unten).

Kommunikation mit Ruben auf **Deutsch**. UI-Texte auf Deutsch.

## Hardware und System

- GIGA R1 WiFi an **COM6** (USB VID `2341`, PID `0266`), FQBN `arduino:mbed_giga:giga`, Core **4.6.0**.
- Display quer **800×480**, 3,97″ ≈ **235 ppi** → 1 px ≈ 0,11 mm. Mindestschrift 18 px.
- PC: i5-12600K, RTX 5070, ASUS ROG STRIX B660-G, 2× DDR5 Corsair. HWiNFO64 8.52 (`C:\Program Files\HWiNFO64`).
- Arduino-Sketchbook liegt in **`OneDrive\Dokumente\Arduino`** (Dokumente umgeleitet); `arduino-cli.exe`
  aus der IDE: `%LOCALAPPDATA%\Programs\arduino-ide\resources\app\lib\backend\resources\`.

## Aufbau

```
Dashboard/            Arduino-Sketch (LVGL 9.2)
  Dashboard.ino       setup/loop (Arduino_H7_Video.h MUSS vor lvgl.h stehen, s. u.)
  config.h            Temperatur-Skalen (min/max/warn/hot), Timeout
  theme.h/.cpp        Design-Tokens (Farben, Schriftrollen)
  model.h             Zustand vom PC (Zeit gültig, Temperaturen, Auslastung, Song)
  link.h/.cpp         serielles Protokoll (PROTOCOL.md)
  cover.h/.cpp        Cover aus Base64-Stücken zusammensetzen (SDRAM)
  display.h/.cpp      Display-Start, eigener Flush, LVGL-Zusatzpool
  diag.h/.cpp         Absturz-/Hänger-Erkennung mit Neustart und Grund
  ui.h/.cpp           ein Screen, 50-ms-Takt
  backdrop.h/.cpp     Hintergrund + Zifferblatt in einem RGB565-Canvas: schwarz oder Cover (unscharf,
                      dunkel), Überblendung, Zifferblattflächen 50 % durchsichtig, geglätteter Rand
  page_clock.cpp      Zifferblatt (einmal gezeichnet, farbig + Flächenmaske, an backdrop übergeben)
                      + Temperatur-Register + Zeiger
  panel.cpp           rechte Spalte: Song-Karte, Wochentag, Datum, Digitalzeit, Auslastung, Hinweis
  src/fonts/          generierte Bahnschrift-Fonts (nicht von Hand ändern)
bridge/               PC-Bridge (Python 3.13), requirements.txt
  bridge.py           Serial (einziger Thread am Port), Zeit/Sensoren/Song jede Sekunde, Cover, Debug-Kanal
  hwinfo.py           HWiNFO Shared Memory (Fallback: Gadget-Registry): Temperaturen + Auslastung
  media.py            Spotify über Windows-Mediensteuerung (eigener Thread), Cover → 96×96 RGB565
  config.json         Port, Sensor-Regex (sensors = Temperaturen, loads = Auslastung), media
  logs/bridge.log     Log (rotierend)
  backups/            Alt: SignalRGB-Registry-Sicherungen aus den Lüftertests (kann weg)
lib/lvgl              LVGL 9.2.2, nur für dieses Projekt gepinnt
tools/build.ps1       kompilieren / hochladen
tools/build_fonts.py  Fonts erzeugen
PROTOCOL.md           Nachrichten GIGA ↔ PC
screenshots/          Debug-Screenshots vom Display
```

## Bauen, Hochladen, Starten

- Kompilieren: `powershell -ExecutionPolicy Bypass -File tools\build.ps1`
- Hochladen: `… tools\build.ps1 -Upload` (pausiert die Bridge über `bridge\.pause`, gibt COM6 frei)
- Bridge-Pakete: `python -m pip install -r bridge\requirements.txt` (pyserial, winrt-…, Pillow).
- Bridge: Autostart-Aufgabe „GIGA Dashboard Bridge“ (`bridge\install_autostart.ps1` / `uninstall_autostart.ps1`);
  manuell `bridge\start_bridge.bat` oder `python bridge\bridge.py` (Konsole mit Log).
  Vor manuellem Start: `Stop-ScheduledTask "GIGA Dashboard Bridge"` (sonst belegt die Aufgabe COM6).
- Fonts neu erzeugen: `python tools\build_fonts.py` (danach neu kompilieren). Erster LVGL-Build ~5 min.
- **Screenshot vom echten Display:** leere Datei `bridge\.shot` anlegen → PNG in `screenshots/`.
  Nach jeder UI-Änderung zur Design-Kontrolle nutzen. Beliebiger Befehl: Datei `bridge\.cmd` (z. B. `MEM`).
  **Jeder zweite Screenshot** ist langsam (~9 s statt ~1 s, Dauer steht im Log) und um 32 px nach oben
  versetzt (ein USB-Paket fehlt) – nur das Bild, nicht die Anzeige. Dann einfach noch einen machen.
- Testwerte beim Konsolenstart (Umgebungsvariablen, `-` = keine Daten): `GIGA_FAKE_TEMPS=52,44,39`,
  `GIGA_FAKE_LOADS=34,-,58`, `GIGA_FAKE_MEDIA=Titel|Interpret|bild.png` (Bild optional) oder
  `GIGA_FAKE_MEDIA=-` (nichts spielt).

## Umgebungsfallen (verifiziert)

1. **LVGL 9.6.0 im Sketchbook ist mit Core 4.6.0 inkompatibel** (Core-`lv_conf_9.h` von v9.0.1 setzt
   `LV_USE_DRAW_ARM2D_SYNC 1` → `arm_2d.h` fehlt). Projekt baut immer mit `--library lib\lvgl` (9.2.2).
   Nicht auf ≥ 9.3 aktualisieren, solange der Core keine passende `lv_conf` hat.
2. Globale `Arduino_GigaDisplayTouch` im Sketchbook enthält eine Altdatei `src/Arduino_GigaDisplayTouch.cpp`
   (v1.0) → „multiple definition“. Für dieses Projekt irrelevant (kein Touch). Globale Libraries nicht ohne
   Rubens ausdrückliche Zustimmung ändern (Rechteprüfung hat es auch abgelehnt).
3. **Smart App Control** blockiert kompilierte Python-Erweiterungen → `pip install --no-binary fonttools fonttools`.
4. Core-`lv_conf` ist fest: `LV_MEM_SIZE` 64 KB, `LV_USE_FLOAT 0`, nur Montserrat 14, keine komprimierten Fonts.
   Canvas-Puffer per `SDRAM.malloc` (erst nach `Display.begin()`).
5. `Dashboard.ino` muss `Arduino_H7_Video.h` vor `lvgl.h` einbinden, sonst findet die Library-Erkennung
   von arduino-cli `lv_conf.h` nicht.

## Stabilität und Diagnose

- **Eigener Flush-Callback** (`display.cpp`): Der Flush von `Arduino_H7_Video` `realloc()`t pro Flush einen
  Rotationspuffer; das führte zu HardFaults in `rotate270_rgb565`. Jetzt fester 38,4-KB-Puffer + `dsi_lcdDrawImage`.
- **Ausrichtung:** `DISPLAY_FLIPPED` in `config.h` (seit 30.09. = 1, um 180° gedreht → LVGL-Rotation 90;
  0 = Arduino-Standard 270). Flush berechnet die Framebuffer-Position je Rotation; `SHOT` meldet die Rotation,
  die Bridge speichert zusätzlich `…-panel.png` (Panel wie montiert, zur Kontrolle der Ausrichtung).
- LVGL-Zusatzpool 64 KB. **TLSF lehnt Pools > 64 KB still ab** (an `LV_MEM_SIZE` gekoppelt). Bei Mangel hängt
  `LV_ASSERT_MALLOC` in `while(1)`.
- `diag.cpp`: fataler mbed-Fehler → `mbed_error_hook` merkt Status/PC/LR; Software-Watchdog (8 s ohne
  `loop()`) → Neustart. Grund im nächsten `HELLO` → Warnung im Bridge-Log („GIGA restarted after a problem“).
  Adresse auflösen: `…\arm-none-eabi-gcc\7-2017q4\bin\arm-none-eabi-addr2line.exe -f -C -e build\Dashboard.ino.elf <pc>`.
  Phasen: 1 setup, 2 link_poll, 3 lv_timer_handler, 4 idle, 5 Screenshot.
  Notiz liegt in `.pdm_buffer` (SRAM4, NOLOAD) – RTC-Backup-Register haben dafür nicht funktioniert
  (die RTC selbst läuft durch einen Reset weiter, s. u.); vor dem Reset `SCB_CleanDCache()`. Kein Hardware-Watchdog (würde DFU-Uploads abbrechen).
- `Serial.write` im mbed-Core blockiert ohne Timeout, bis der PC liest → die Bridge liest ständig.
  Nur ein Thread fasst den Port an (früher: Port-Close aus zweitem Thread hat Bridge und GIGA-USB festgefahren);
  `media.py` läuft in einem eigenen Thread, liefert aber nur Daten ab. Der Screenshot meldet sich pro 4-KB-Stück
  beim Watchdog (ein langsamer Screenshot dauert ~8 s).
- **SDRAM-Lesefehler (gemessen 01.10.):** Beim Lesen aus dem SDRAM kippt unter Last gelegentlich ein
  Bit, immer auf derselben Datenleitung (vermutlich D6, seltener D5). In RGB565 trifft das nur die untersten
  Grünbits (unsichtbar, war schon immer so); ein ARGB8888-Zifferblatt bekam Bit 6 im Rotkanal ab →
  10–50 rosa Punkte am Rand nach jeder Überblendung. Darum: **Puffer, die LVGL bei jedem Bild aus dem SDRAM
  liest, nur als RGB565** – Zifferblatt und Hintergrund liegen gemeinsam in einem RGB565-Canvas, den Rand
  rechnet `backdrop.cpp` selbst auf den Hintergrund. Rechenwerte (Raster, Zeilenpuffer, Flächenmaske mit
  1 Bit/Pixel) im internen RAM.
- Empfang: USB-Ringpuffer des Cores nur 256 Byte (Flusskontrolle per NAK, nichts geht verloren);
  `link_poll()` nimmt höchstens 2 KB pro Durchlauf, damit ein Cover (~26 KB) die UI nicht anhält.

## Uhrzeit und RTC

- Zeitbasis ist die RTC des STM32 (LSE 32,768 kHz, `time()`/`set_time()` von mbed). Die Bridge stellt sie mit
  jedem `T` (jede Sekunde, Lokalzeit als UTC); ohne Verbindung läuft die Uhr einfach mit der RTC weiter.
- Die RTC übersteht Upload, Watchdog-Neustart und Reset (verifiziert 30.09.: Abweichung danach 0 s; 6 min ohne
  Bridge ebenfalls 0 s → läuft wirklich am Quarz, nicht am ungenauen LSI). Beim Start
  zeigt `link_begin()` die RTC-Zeit sofort an, wenn sie ≥ `RTC_VALID_FROM` (config.h) ist; sonst Ruhestellung
  10:10 und „--:--“ bis zur Bridge.
- Stromlos geht die RTC verloren, außer eine Knopfzelle hängt am Pin **VRTC** des GIGA.
- `SYNC|<abw>|<offline>` (erstes `T` nach Start oder Abbruch) → Bridge-Log „GIGA clock resynced … off by …“.
- Grenze: Ein Sommerzeitwechsel ohne PC-Verbindung wird erst beim nächsten Kontakt korrigiert (der GIGA kennt
  nur Lokalzeit, keine Zeitzonenregel).

## Sensoren

HWiNFO64 mit **„Shared Memory Support“** (Free-Version: 12 h, dann neu aktivieren) und laufendem
Sensorfenster (minimiert reicht). Bereich `Global\HWiNFO_SENS_SM2`; nach dem Aktivieren HWiNFO neu starten.
Alternative ohne Limit: pro Sensor „Report value in Gadget“ (Registry `HKCU\Software\HWiNFO64\VSB`).
Zuordnung (geprüft 30.09.): CPU = „CPU [#0] … CPU Package“, GPU = „dGPU [#0] … GPU Temperature“,
RAM = heißester „DDR5 DIMM … SPD Hub Temperature“.

## Auslastung und Spotify

- Auslastung aus HWiNFO (`loads` in config.json, alles mit Einheit %): CPU = „CPU [#0] … Total CPU Usage“,
  GPU = „dGPU [#0] … GPU Core Load“, RAM = „System … Physical Memory Load“ (geprüft 01.10.).
- Song: `media.py` liest die Windows-Mediensteuerung (GlobalSystemMediaTransportControls, wie das
  Medien-Overlay von Windows) – **kein Spotify-Konto/API-Schlüssel nötig**. Sitzung der App, deren ID einen
  Eintrag aus `media.apps` enthält (Store-Spotify: `SpotifyAB.SpotifyMusic_…!Spotify`). Nur bei Status
  „Playing“; nach Pause/Stopp verschwindet die Karte nach `hide_after_s` (3 s, überbrückt Titelwechsel).
- Cover: Spotify liefert 300×300 PNG; die Bridge schneidet quadratisch zu, skaliert auf 96×96 (Lanczos),
  wandelt in RGB565 und schickt es in 128 Base64-Zeilen; der GIGA bestätigt mit `COVER`. Gleiches Cover
  (gleiches Album) wird nicht erneut übertragen. Nach einem Titelwechsel liest die Bridge das Cover 6 s lang
  jede Sekunde neu, weil Apps es manchmal erst nach dem Titel aktualisieren.
- Zeichen: Titel/Interpret nur aus `font_ui_22` (Latin-1 + Latin Extended-A + Satzzeichen, `MEDIA_RANGES`
  in `tools/build_fonts.py` = `_RANGES` in `bridge/media.py`, beide gleich halten). Andere Akzente werden
  abgelöst, Emoji entfallen, andere Schriften werden „?“.
- Ohne Verbindung zum PC (> 5 s) blendet der GIGA die Karte aus.

## Warum kein Touch

Touch-Bus `Wire1` (SDA PH12 / SCL PB6) liegt dauerhaft auf Low, schon vor jeder Initialisierung und auch
gegen Pull-up; kein Teilnehmer antwortet (GT911, BMI270, LED-Treiber, Krypto-Chip). Das **unveränderte
offizielle Beispiel `Touch_Polling`** (GitHub, 1.1.2) meldet ebenfalls „init - FAILED“; Stromtrennen hilft
nicht → Hardwaredefekt am Shield/Bus. Nicht erneut per Software versuchen, ohne dass die Hardware geprüft
oder getauscht wurde. (Randnotiz: Option Bytes BCM4=1, M4 startet ab 0x08100000 ein ungültiges Image und
bleibt sofort stehen – harmlos.)

## Design-System

Konzept: **ein nachtblaues Chronographen-Zifferblatt in DIN statt Gamer-HUD**, auf tiefem Schwarz; spielt
Spotify, liegt das Album-Cover leicht unscharf und abgedunkelt dahinter. Die Flächen von Zifferblatt und
Registern lassen 50 % des Hintergrunds durch (auf Schwarz also 50 % dunkler), Teilstriche, Ziffern, Bögen
und Randlinie bleiben deckend. Die drei Register sind die
Temperaturen (CPU 9 Uhr, GPU 3 Uhr, RAM 6 Uhr) mit Bernstein-/Signalrot-Bogen ab warn/hot. Sekundenzeiger
springt wie ein Quarzwerk (160 ms, leichter Überschwinger), Register-Zeiger gleiten (600 ms).

| Token | Hex | Verwendung |
|---|---|---|
| Schwarz | `#000000` | Hintergrund (ohne Musik) |
| Lack | `#0A1522` | Kontur der Zeiger, Zentrum |
| Zifferblatt | `#102036` | Zifferblatt (Fläche 50 % durchsichtig) |
| Register | `#0C1A2C` | vertiefte Register (Fläche 50 % durchsichtig) |
| Teilung | `#26395A` | Haarlinien, kleine Teilstriche |
| Leuchtmasse | `#ECE5D0` | Zeiger, Ziffern, Primärtext |
| Schiefer | `#8290AA` | Sekundärtext, Beschriftungen |
| Bernstein | `#F2A33A` | Warnung (Temperatur ≥ warn), Hinweise |
| Signalrot | `#F2544B` | heiß |
| Akzent | `#4DB5EB` | Sekundenzeiger, Digitalsekunden |

Schrift: **Bahnschrift** (Microsofts DIN 1451), aus der Windows-Variable-Font geschnitten.
Display = SemiBold Condensed (120/48/40 px, nur Ziffern), Text = SemiLight (34/24 px),
Hinweise und Song-Karte = SemiCondensed Regular 22 px (mit Latin-1/Latin Extended-A), Register- und
Auslastungsnamen = SemiCondensed SemiBold 18 px, +2 Sperrung. Auslastungswerte = Display 40 px (mit „%“).
Die generierten Fonts stammen aus einer Microsoft-Schrift: **nur privat nutzen, nicht veröffentlichen.**

Layout: Zifferblatt 440 px bei (24,20), Mitte y=240. Rechte Spalte x=500–776 (276 px), immer auf die
Zifferblattmitte zentriert (`panel.cpp`):
- Block (272 px hoch, relativ): Wochentag 0, Datum 44, Zeit 88, Auslastung 205 (Name, Wert 40 px, Balken 6 px;
  drei Spalten à 80 px im Abstand 98). Ohne Musik Block-Oberkante y=104.
- Spielt Spotify, gleitet der Block in 400 ms auf y=167, danach erscheint darüber die Song-Karte (y=41):
  Cover 96 px mit Haarlinie (Platzhalter: leeres Register-Feld), daneben Titel (Leuchtmasse, max. 2 Zeilen)
  und Interpret (Schiefer), zusammen max. 3 Zeilen, Rest mit „...“, Abstand 28 px zwischen allen Teilen.
- Hinweiszeile nur bei Problemen; sie ersetzt die Auslastungszeile (deren Werte fehlen dann ohnehin).
- Cover-Hintergrund (`backdrop.cpp`): Cover-Mittelstreifen (5:3) auf ein Raster 48×29 (je 2×2 Pixel),
  eine Binomial-Unschärfe (1 4 6 4 1), Sättigung ×1,17, Helligkeit ×0,47, gedeckelt auf Luma 50 und
  Kanal 110 (von 255; helle Cover werden dunkles Grau, Text bleibt lesbar), bilinear aufs Display,
  4×4-Bayer-Dithering nach RGB565. Wechsel blenden in 600 ms über (smoothstep, ~18 Bilder/s, Vollbild,
  weil das Zifferblatt durchscheint). Neues Album: das alte bleibt, bis das neue Cover da ist. Schwarz bei Pause/Stopp, Titel ohne Cover und ohne PC-Verbindung. Parameter oben in `backdrop.cpp` (`SHOW_THROUGH` = Durchsicht des Zifferblatts).
- Auslastungsbalken: Spur in Teilung, Füllung in Leuchtmasse, gleiten 600 ms wie die Register-Zeiger.

## Regeln für Code-Änderungen

- Farben und Schriften nur über `theme.h`. Keine Montserrat, keine Hex-Werte in Seiten-Dateien.
- Kein `style_opa` < 255, keine Transformationen (brauchen Layer-Puffer aus dem kleinen Heap).
- Statisches einmal in einen Canvas (SDRAM, **nur RGB565**, s. SDRAM-Lesefehler) zeichnen; Bewegtes per
  `LV_EVENT_DRAW_MAIN` und gezieltem `lv_obj_invalidate_area` (nur Zeiger-Bounding-Boxen).
- `loop()` nie blockieren. Serial nur schreiben, wenn `Serial` (DTR) aktiv ist.
- UI-Texte: Deutsch, Satzanfang groß; Hinweise sagen, was zu tun ist; im Normalfall keine Statuszeile.
- Protokolländerungen immer gleichzeitig in `PROTOCOL.md`, `bridge/bridge.py`, `Dashboard/link.cpp`.
- Keine globalen Einstellungen (Arduino-Libraries, Autostart, Option Bytes) ohne Rubens Zustimmung ändern.

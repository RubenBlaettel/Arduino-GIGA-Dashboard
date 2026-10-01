# Serielles Protokoll GIGA ↔ PC-Bridge

USB-Seriell (COM6, 115200), UTF-8, eine Nachricht pro Zeile (`\n`), Felder durch `|` getrennt.
`-` steht für „unbekannt“.

Implementiert in `bridge/bridge.py` (PC) und `Dashboard/link.cpp` (GIGA), Cover-Zusammenbau in
`Dashboard/cover.cpp`.
**Jede Änderung hier muss in beiden Dateien gleichzeitig erfolgen.**

## PC → GIGA

| Nachricht | Bedeutung | Wann |
|---|---|---|
| `T\|<epoch>` | Lokalzeit als Unix-Sekunden (Zeitzone/Sommerzeit schon eingerechnet); stellt die RTC des GIGA. Ohne Verbindung läuft die Uhr mit der RTC weiter. | direkt nach jeder vollen Sekunde |
| `S\|<cpu>\|<gpu>\|<ram>` | Temperaturen in °C, eine Nachkommastelle, `-` = keine Daten | jede Sekunde |
| `L\|<cpu>\|<gpu>\|<ram>` | Auslastung in %, ganzzahlig 0–100, `-` = keine Daten | jede Sekunde |
| `P\|<cover>\|<titel>\|<interpret>` | Spotify spielt. `<cover>` = Cover-ID (8 Hex-Ziffern, CRC32 der Pixel, nie 0) oder `-` ohne Cover. Titel und Interpret je höchstens 120 Byte UTF-8, nur Zeichen aus dem Font (`MEDIA_RANGES`), `\|` wird zu `/`. | sofort bei Änderung und jede Sekunde |
| `P` | Es läuft nichts (Pause/Stopp länger als `hide_after_s`, Spotify zu) | wie oben |
| `A\|<cover>\|<n>\|<base64>` | Cover-Stück `n` (0–127): 144 Byte (192 Zeichen Base64, ohne Padding) des Bilds 96×96 RGB565 little-endian, zeilenweise | nach einem `P` mit neuem Cover, 8 Zeilen pro Bridge-Durchlauf; ohne `COVER` nach 3 s erneut (max. 3 Versuche) |

## GIGA → PC

| Nachricht | Bedeutung |
|---|---|
| `HELLO\|<fw>\|<reset>` | Einmal pro Boot nach der ersten empfangenen Zeile, sonst alle 3 s solange keine Daten kommen. `<reset>` ist leer oder nennt den Grund des letzten Neustarts (`hang phase=N` / `fault status=… pc=… lr=… phase=N`); die Bridge loggt ihn als Warnung. |
| `COVER\|<cover>` | Cover vollständig empfangen (auch nach einer erneuten Übertragung). Die Bridge schickt dieses Cover dann nicht mehr, bis der GIGA neu startet (`HELLO`) oder die Verbindung neu aufgebaut wird. |
| `SYNC\|<abw>\|<offline>` | Beim ersten `T` nach dem Start oder nach einem Verbindungsabbruch (≥ 5 s ohne Daten). `<abw>` = RTC minus PC-Zeit in Sekunden vor dem Nachstellen (Auflösung 1 s; positiv = GIGA ging vor), `-` = RTC hatte keine gültige Zeit. `<offline>` = Sekunden ohne Daten vom PC (nach einem Start: seit dem Start). Die Bridge loggt es, ab > 2 s als Warnung. |

## Debug

| Richtung | Nachricht | Bedeutung |
|---|---|---|
| PC → GIGA | `SHOT` | Screenshot anfordern |
| GIGA → PC | `SHOT\|<w>\|<h>\|<rot>` + w·h·2 Bytes RGB565 | Framebuffer, physisch hochkant (480×800); `<rot>` = 90 (gedreht) oder 270 (Arduino-Standard), danach dreht die Bridge ins Querformat. Bekannte Macke: Jeder zweite Screenshot dauert ~9 s statt ~1 s und verliert am Anfang ein USB-Paket (Bild um 32 px nach oben versetzt, unten ein Streifen) – dann einfach noch einen machen. |
| PC → GIGA | `MEM` | Speicher abfragen |
| GIGA → PC | `MEM\|<total>\|<frei>\|<größter>\|<max>\|<%>\|<frag%>\|<malloc>` | LVGL-Pools + größter freier `malloc`-Block |
| PC → GIGA | `DIAG\|hang` / `DIAG\|fault` | Hänger bzw. Absturz erzwingen, um die Selbstheilung zu testen |

Auslösen: Datei `bridge/.shot` anlegen (Screenshot → `screenshots/`) oder `bridge/.cmd` mit einer
Befehlszeile (z. B. `MEM`). Die laufende Bridge holt die Datei ab und löscht sie.

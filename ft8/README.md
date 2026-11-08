# Eigenständiger FT8-Decoder für SDR-Projekte

Dieses Projekt extrahiert den normalen FT8-Decoder aus
[WSJT-X](https://github.com/WSJTX/wsjtx/tree/967c85a6119110788b0df3782cf796485b2cee14).
Das Programm `ft8-decoder` verarbeitet einen Audioblock und schreibt die
Ergebnisse auf stdout. Es benötigt weder die WSJT-X-Anwendung noch Qt,
Soundkarte, Funkgerätesteuerung, Shared Memory oder Netzwerkzugriff.

Der Decoder ist als eigenständiges Unterprojekt in kwWebRXpp enthalten.
Im übergeordneten kwWebRXpp-Projektverzeichnis baut `make` beide Programme;
`make ft8` baut nur den Decoder. Erste Aufrufe aus diesem Projektverzeichnis:

```sh
make ft8
./ft8-decoder --utc 133430 ft8/tests/data/210703_133430.wav
./ft8-decoder --output text ft8/tests/data/210703_133430.wav
./ft8-decoder --help
make ft8-test
```

Der Build erzeugt `build/ft8-decoder` in diesem Unterverzeichnis und eine
Kopie neben `kwWebRXpp`. Alle weiteren direkten CMake-Befehle und Beispiele
mit `./build/ft8-decoder` beziehen sich auf dieses `ft8/`-Unterverzeichnis.
Ein externes Decoder-Verzeichnis wird weder beim Bauen noch zur Laufzeit benötigt.

## Build und Tests

Benötigt werden CMake >= 3.20, ein C++17-Compiler, GNU Fortran,
FFTW3 in einfacher Genauigkeit und die Boost-Header für CRC14.
Python >= 3.8 wird für Tests und das optionale Integrationsbeispiel gebraucht.
Unter Debian/Ubuntu lassen sich die Build-Abhängigkeiten so installieren:

```sh
sudo apt install build-essential gfortran cmake pkg-config libfftw3-dev libboost-dev python3
```

Danach, auch offline und ohne WSJT-X-Checkout:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
python3 tools/verify_vendor.py
```

Zum Bauen ohne Tests und ohne Python-Abhängigkeit beim Konfigurieren:

```sh
cmake -S . -B build-minimal -DBUILD_TESTING=OFF
cmake --build build-minimal -j
```

Optionale Installation, beispielsweise in ein Verzeichnis im eigenen Projekt:

```sh
cmake --install build --prefix /dein/projekt/ft8-runtime
```

Das Binary ist dynamisch gelinkt und benötigt zur Laufzeit die üblichen
C++-/Fortran-Laufzeitbibliotheken sowie `libfftw3f`. Getestete Plattform:
Linux x86_64, Ubuntu 24.04, GCC/GFortran 13.3, FFTW 3.3.10 und Boost 1.83.

## Eingang und zeitliche Ausrichtung

Pro Aufruf wird **ein FT8-Slot** verarbeitet:

- Mono-Audio mit exakt **12.000 Samples/s**.
- Normalerweise 180.000 Samples, also 15 Sekunden. 13 bis 15 Sekunden werden
  akzeptiert; fehlende Samples am Ende ergänzt das Programm mit Nullen.
- Der erste Sample gehört zum Beginn eines UTC-Slots bei Sekunde 00, 15, 30
  oder 45. Der normale FT8-Sendebeginn liegt rund 0,5 Sekunden danach.
- Formate: `wav` (Standard; unkomprimiertes RIFF/WAVE mit PCM16), `s16le`
  (vorzeichenbehaftete 16-Bit-Werte, Little Endian) und `f32le`
  (IEEE-754-Floats, Little Endian, endlich und auf [-1,1] normiert).
- WAV-Metadaten und zusätzliche RIFF-Chunks werden berücksichtigt. Stereo,
  andere Sampleraten und andere WAV-Codierungen werden mit einer Fehlermeldung
  abgelehnt. Es erfolgt kein automatisches Resampling.

Rohe IQ-Daten müssen im SDR-Projekt zuerst zu Audio demoduliert werden.
Eine typische Empfangskette ist: IQ → USB-Demodulation → Resampling auf
12 kHz → Mono-Audio in UTC-Slots teilen → Decoder aufrufen. `frequency_hz`
bezieht sich auf dieses Audio. Die HF-Frequenz ergibt sich je nach
Demodulation und Abstimmung im SDR-Projekt.

`--utc HHMMSS` setzt ausschließlich die ausgegebene Zeitmarke. Diese Option
verschiebt das Audio nicht und synchronisiert keine Aufnahme.

Dateien und Pipes funktionieren gleich:

```sh
./build/ft8-decoder aufnahme.wav
./build/ft8-decoder --format s16le --utc 120015 slot.raw
./build/ft8-decoder --format f32le - < slot.f32
```

Bei `-` wird stdin **bis EOF** gelesen. Der aufrufende Prozess muss seine
Schreibseite der Pipe schließen, damit die Decodierung beginnt. Ein Aufruf
verarbeitet einen Block und beendet sich danach. Für den nächsten Slot wird
ein neuer Prozess gestartet. Ein dauerhafter Audiostream muss vorher im
SDR-Projekt in Slots aufgeteilt werden.

## Ausgabe und Aufrufprotokoll

Standard ist JSON Lines: genau ein JSON-Objekt pro Zeile. Zuerst kommen
`decode`-Objekte, danach immer genau ein `complete`-Objekt. Bei erfolgreicher
Verarbeitung ohne Empfang kommt nur das `complete`-Objekt. stdout enthält
keine Statusmeldungen; Fehler stehen auf stderr.

Beispiel aus der mitgelieferten Originalaufnahme:

```json
{"type":"decode","utc":"133430","snr_db":16,"dt_s":0.255,"frequency_hz":2571.375,"sync":238.405,"quality":0.916,"ap":0,"message":"W1FC F5BZB -08"}
```

Bei dieser Aufnahme folgen weitere decodierte Nachrichten. Die letzte Zeile
lautet im getesteten Release-Build:

```json
{"type":"complete","decodes":21,"samples":180000,"sample_rate":12000}
```

| Feld | Bedeutung |
| --- | --- |
| `utc` | Übergebenes HHMMSS-Label; ohne `--utc` ist es `null` |
| `message` | Vollständiger decodierter FT8-Nachrichtentext, bis 37 Zeichen |
| `snr_db` | WSJT-X-SNR-Schätzung in dB, bezogen auf 2.500 Hz Bandbreite |
| `dt_s` | Zeitversatz relativ zum normalen Sendebeginn 0,5 s nach Slotanfang |
| `frequency_hz` | Geschätzte Frequenz des untersten FT8-Tons im Audio |
| `sync`, `quality` | Unveränderte WSJT-X-Diagnosewerte; keine Fehlerwahrscheinlichkeit |
| `ap` | AP-Decodiertyp; bei dieser zustandslosen Schnittstelle 0 |
| `decodes` | Anzahl ausgegebener `decode`-Objekte |
| `samples` | Tatsächlich eingelesene Samplezahl vor dem Auffüllen |
| `sample_rate` | Immer 12000 |

Die Reihenfolge der Nachrichten hängt von den Decoder-Durchläufen ab.
Identische Nachrichtentexte innerhalb eines Slots werden wie im
WSJT-X-Decoder zusammengefasst, auch wenn sie auf mehreren Frequenzen vorkommen.

`--output text` gibt ausschließlich lesbare Decode-Zeilen aus:
`UTC SNR DT Frequenz ~ Nachricht`. Ein Slot ohne Decode erzeugt in diesem
Modus keine stdout-Ausgabe.

Exitcodes: `0` = erfolgreich verarbeitet, auch ohne Nachrichten;
`2` = ungültige Argumente oder Eingabedaten; `3` = interner/Laufzeitfehler.
Das SDR-Projekt sollte einen Timeout und die Auswertung des Exitcodes vorsehen.

## Decoder-Optionen und Grenzen

```sh
./build/ft8-decoder --depth 3 --min-freq 200 --max-freq 4000 aufnahme.wav
```

`--depth` entspricht den WSJT-X-Stufen 1 (schnell), 2 und 3 (gründlich,
Standard). Der Suchbereich ist standardmäßig 200–4000 Hz. Er muss innerhalb
100–4900 Hz liegen und mindestens 200 Hz breit sein, damit die ursprüngliche
Spektral-Baseline-Schätzung ausreichend Punkte hat. Die Grenzen steuern die
Suche nach dem untersten Ton; dessen feinbestimmte Frequenz kann geringfügig
von einer Grenze abweichen.

Verwendet werden der originale FT8-Synchronisierer, LDPC/BP/OSD,
CRC-Prüfung, 77-Bit-Nachrichtenentpackung und Mehrfachdurchläufe mit
Signal-Subtraktion. Die drei Empfangsphasen des Originaldecoders werden
für den Audioblock nacheinander ausgeführt. Zeitabhängige Abbruchbedingungen
sind über dessen Offline-Modus deaktiviert, damit das Ergebnis nicht von der
aktuellen Uhrzeit abhängt.

AP-Decodierung mit eigenem Rufzeichen, QSO-Verlauf oder früheren Slots ist
in dieser Schnittstelle deaktiviert. Hash-Rufzeichen ohne auflösbaren
Rufzeichenkontext können deshalb als `<...>` erscheinen. Dies ist der normale
FT8-Pfad aus `lib/ft8_decode.f90`, nicht die separate alternative
`ft8var`-Implementierung und nicht FT4, WSPR oder andere Betriebsarten.

Der interne Fortran-Code verwendet globale Zustände und ist nicht threadsicher.
Der öffentliche Integrationsweg ist das ausführbare Programm: Ein Prozess
pro Slot; für parallele Aufrufe getrennte Prozesse verwenden. Die interne
C/Fortran-Brücke ist keine unterstützte Bibliotheks-API für wiederholte Aufrufe.

## Integration aus Python

Ein vollständiges Beispiel steht in `examples/decode_slot.py`:

```python
from examples.decode_slot import decode_pcm16

# pcm_bytes: 180000 Mono-Samples als signed int16 Little Endian bei 12000 Hz
for result in decode_pcm16(pcm_bytes, utc="120015"):
    print(result["message"], result["snr_db"], result["frequency_hz"])
```

Ohne Hilfsmodul entspricht der Aufruf diesem Muster:

```python
import json
import subprocess

p = subprocess.run(
    ["./build/ft8-decoder", "--format", "s16le", "-"],
    input=pcm_bytes,
    capture_output=True,
    timeout=45,
    check=True,
)
rows = [json.loads(line) for line in p.stdout.splitlines()]
assert rows[-1]["type"] == "complete"
decodes = [row for row in rows if row["type"] == "decode"]
```

Dasselbe Protokoll kann aus C++, Rust, Java oder anderen Sprachen über einen
Unterprozess mit stdin/stdout verwendet werden. Eine Shell ist dabei nicht
nötig. stderr sollte ebenfalls gelesen werden, um Prozessfehler zu melden.

## Herkunft und Änderungen

Festgehaltener WSJT-X-Commit:
`967c85a6119110788b0df3782cf796485b2cee14`.
`vendor/wsjtx/UPSTREAM.json` dokumentiert Quellpfade und SHA-256-Prüfsummen.
Die übernommenen Dateien sind unverändert. Zusätzlich wurde ausschließlich
die `stdcall`-Routine aus `lib/qra/q65/q65_set_list.f90` extrahiert, ohne den
unabhängigen Q65-Encoder mitzunehmen.

Beim Konfigurieren erzeugt CMake eine Kopie von `ft8_decode.f90` und entfernt
dort den unbenutzten Import von `shmem_lock`/`shmem_unlock`. Das vermeidet die
GUI-/IPC-Abhängigkeit. Die originale Quelldatei bleibt im Vendor-Verzeichnis
unverändert. Die ursprüngliche No-op-Implementierung von `timer_module`
bleibt erhalten. Neu sind CMake-Build, Fortran/C-Brücke, Audioleser, CLI,
Dokumentation und Tests.

Eine reproduzierbare erneute Extraktion aus einem lokalen, unveränderten
Checkout desselben Commits ist möglich:

```sh
python3 tools/vendor_wsjtx.py /pfad/zum/wsjtx-checkout
python3 tools/verify_vendor.py
```

Die Integrationsprüfungen testen die Originalaufnahme, deterministisch
erzeugte Signale mit bekanntem Inhalt, SNR, Frequenz und positivem/negativem
Zeitversatz, gleichzeitige Signale, alle drei Decoderstufen, WAV mit
Metadaten, s16le/f32le über stdin, Stille, Suchbereiche und fehlerhafte Eingaben.
Die Testerzeugung verwendet den FT8-Encoder ausschließlich für Testsignale.

## Lizenz

Der extrahierte WSJT-X-Code steht unter GPLv3; der vollständige Lizenztext
liegt in `COPYING`, die Autorenliste in `vendor/wsjtx/AUTHORS`. Die neuen
Integrationsdateien dieses Projekts sind ebenfalls mit GPL-3.0-only markiert.
Herkunft und Lizenz müssen bei einer Weitergabe erhalten bleiben.

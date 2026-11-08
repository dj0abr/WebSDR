# WebSDR for SDRplay RSP1A and RSP1B

WebSDR (`kwWebRXpp`) turns an SDRplay RSP1A or RSP1B into a receiver that can
be used from desktop and mobile web browsers. Multiple listeners share the
selected RF band and can independently tune, select USB/LSB/FM and adjust their
audio, including speech noise reduction.

**Live receiver:** [DB0SL WebSDR](http://sdr.db0sl.de).

The original 2024 version remains available on the
[`legacy-v1` branch](https://github.com/dj0abr/WebSDR/tree/legacy-v1).

## Features

- Desktop interface with a wideband waterfall and a second waterfall covering
  ±24 kHz around the listener's tuning frequency.
- Mobile interface with touch tuning, audio controls and an FT8 table.
- USB, LSB and FM reception; selectable SSB filters in the desktop settings menu.
- Independent tuning, audio AGC and noise reduction for each logged-in listener.
- Up to 20 listeners; band changes are permitted when exactly one user is logged in.
- Band segments from 630 m through 70 cm, plus 27 MHz CB and PMR446.
- Automatic FT8 decoding within supported band segments, with results in the
  terminal and both browser interfaces, including callsign-based country lookup.
- ADS-B reception at 1090 MHz, with an aircraft table and map in the desktop interface.

**Live-tested on RSP1A:** the current version, including noise reduction, was
successfully rebuilt on the receiver host with `make clean` followed by `make`.
After restarting the existing receiver service and deploying the updated HTML/JS
files, reception worked immediately. This confirms the RSP1A deployment; it does
not establish that every supported hardware/platform combination has been tested.

## Requirements

- Linux and an SDRplay RSP1A or RSP1B, with the SDRplay API installed separately.
  The documented setup uses API 3.15 or newer, including its development headers
  and a running API service.
- A C++17 compiler, GNU Make, CMake >= 3.20, GNU Fortran (`gfortran`), `pkg-config`,
  Boost headers, FFTW3 development files and zlib development files.
- uWebSockets headers and the uSockets static library installed where the
  Makefile expects them: `/usr/local/include/uWebSockets` and
  `/usr/local/lib/uSockets.a`.
- liquid-dsp: matching headers and libraries are included in `liquid.h` and `lib/`.
  Its shared library must also be available to the runtime loader.
- SpeexDSP for noise reduction: the matching headers and a static Linux x86-64
  library are included in `external/speexdsp-1.2.1/`. Other architectures use
  the system development package (`libspeexdsp-dev`) unless a matching bundle exists.
- Python 3.8 or newer for the FT8 build's test configuration and Python tests.
- A web server for the static browser files, for example Apache, and a browser
  with WebSocket and Web Audio support. The supplied HTML/JS pages do not require PHP.
- Optional: Node.js for browser regression tests; a compatible `readsb` executable
  for ADS-B; an Internet connection in the browser for the ADS-B map assets and tiles.

The Makefile contains library selections for `x86_64`, `aarch64` and `armhf`.
The supplied binaries must match the target CPU and Linux runtime. For a different
platform, provide suitable libraries and adjust the Makefile as needed.

## Build and installation

### 1. Obtain the complete source tree

```sh
git clone https://github.com/dj0abr/WebSDR.git
cd WebSDR
```

When transferring an existing checkout, include **`ft8/`, `external/`, `lib/`
and `html/`** along with the top-level source files. Build on the receiver host
rather than reusing executables or build directories from another computer.

### 2. Install dependencies

On Debian/Ubuntu-based systems, install the distribution packages:

```sh
sudo apt update
sudo apt install git build-essential gfortran cmake pkg-config python3 \
    zlib1g-dev libfftw3-dev libboost-all-dev libspeexdsp-dev apache2
```

Install the Linux SDRplay API from [SDRplay](https://www.sdrplay.com/) and ensure
its service is running and the receiver is accessible to the account running WebSDR.

For uWebSockets/uSockets, the following commands follow the supplied `prepare`
helper. Run them from the project directory when these dependencies are not yet
installed:

```sh
git clone --recurse-submodules --shallow-submodules https://github.com/uNetworking/uWebSockets
cd uWebSockets
make
sudo make install
cd ..
sudo cp uWebSockets/uSockets/src/libusockets.h /usr/local/include/
sudo cp uWebSockets/uSockets/uSockets.a /usr/local/lib/
```

The `prepare` file collects the dependency setup commands; it also runs a system
package upgrade. An existing installation does not need to repeat this setup
for an ordinary source update.

### 3. Build both executables

```sh
make
```

This builds `kwWebRXpp` and the bundled `ft8-decoder`, placing both executables
side by side in the project directory. The FT8 decoder is built from the local
WSJT-X source subset; no separate WSJT-X installation or checkout is needed.
On x86-64, the normal build also uses the local SpeexDSP archive without a download.

Start the receiver from the project directory:

```sh
./kwWebRXpp
```

Check the terminal for device detection and the WebSocket listener on port 9001.
Keep `ft8-decoder` next to `kwWebRXpp` if installing the executables elsewhere.
If `libliquid.so` cannot be found, configure the runtime library search path for
the matching directory under `lib/`, or install the matching shared library.

For an existing systemd deployment, retain its service configuration and executable
path. ADS-B additionally requires the working directory described below.
This repository does not provide a systemd service unit.

### 4. Deploy the browser interface

For Apache's default web root, copy the complete contents of `html/`:

```sh
sudo cp -a html/. /var/www/html/
```

Use the actual document root if your web server has a different configuration.
Deploy these current interface files together:

- `index.html` — desktop interface.
- `fftmobile.html` — mobile interface.
- `noise-reduction.js` — shared NR controls and receiver status handling.
- `ft8-countries.js` and `ft8-country-data.js` — FT8 country lookup.

Open `http://<receiver-host>/` for the desktop interface, or
`http://<receiver-host>/fftmobile.html` for the mobile interface. Reload the page
after an update; use a hard reload if the browser retains older JavaScript.

## Updating an existing receiver

1. Update or copy the complete source tree, including the directories listed above.
2. In the receiver's project directory, rebuild from scratch:

   ```sh
   make clean
   make
   ```

3. Deploy the updated contents of `html/` to the existing web root.
4. Restart the existing receiver service, using its configured service name.
5. Reload desktop/mobile pages and check reception, FT8 results and NR operation.

`make clean` removes the receiver and decoder executables, top-level object and
dependency files, and `ft8/build/`. Both executables are recreated by `make`.
Updating only the web files is sufficient for a browser-only change; the current
NR update needs both the rebuilt receiver and the matching HTML/JS files.

## Connecting and operating

Enter a callsign or listener identifier, select the mode and enable audio.
Identifiers contain 3–32 characters from A–Z, 0–9 and `/`, including at least one
letter, for example `DJ0ABR/P` or `SWL`. Whitespace is trimmed and letters are
converted to uppercase. There is no separate password or callsign-license lookup.
Clearing the identifier disconnects the browser.

Only users with valid login data count toward the user list and band-selection
rule. The server also enforces this rule: when multiple users are logged in,
the RF band is shared and cannot be changed, while each listener can still tune
independently within it. Anonymous connections do not block band changes.

In the desktop interface, click the upper waterfall for coarse tuning and the
lower waterfall for fine tuning. Mouse-wheel steps are 100 Hz on the upper
waterfall, 1 kHz with Shift, 12.5 kHz with Shift+Alt, and 10 Hz on the lower
waterfall. SSB filter choices are available in the settings menu. The mobile
interface provides touch tuning and the same per-listener NR controls.

### Network and reverse proxy setup

The receiver listens for plain WebSocket connections on **TCP port 9001**; the
web server serves the browser files separately. Both interfaces choose their
WebSocket gateway as follows:

| Page hostname | WebSocket destination |
| --- | --- |
| `localhost`, `127.0.0.1`, or a private IPv4 address | `ws://<page-hostname>:9001` |
| A public subdomain, for example `sdr.db0sl.de` | `wss://ws.<main-domain>`, for example `wss://ws.db0sl.de` |
| The public main domain itself | `wss://<main-domain>` |

The public gateway uses the root path, not `/ws`. Configure DNS, TLS and a
WebSocket-capable reverse proxy to match the chosen gateway and forward it to
port 9001. Known multipart suffixes such as `co.uk` retain their main-domain
handling. For direct LAN use, serve the page over HTTP with the receiver's LAN
address. The WebSocket rules are implemented in both HTML files.

## Speech noise reduction

Enable **NR** and adjust **Stärke** in either browser interface. Start around
50–60% and compare by switching NR on and off. The adjacent status confirms the
setting actually applied by the receiver.

- Every new listener connection starts with NR **off**.
- Switching off retains the slider strength for the next comparison.
- NR and audio AGC have separate state for each listener.
- NR operates on USB/LSB speech. FM automatically bypasses it while preserving the
  selected strength for a return to USB/LSB. Leave it off for CW and digital audio.
- The independent FT8 receiver, ADS-B and waterfall paths are unaffected.

The denoiser is adapted from QO100_TRX2 and uses SpeexDSP 1.2.1. It processes
160-sample frames at 8 kHz (20 ms). Noise estimation continues while bypassed;
after a 20 ms crossfade, bypass preserves the original float audio without PCM16
quantization. Active NR adds about 20 ms of algorithmic delay, in addition to
up to one frame of input buffering. The browser audio format remains unchanged.
Ordinary tuning preserves the estimate; mode/band changes reset NR history and
discard buffered old audio.

The strength sets a maximum noise-suppression level up to 30 dB, rather than a
guaranteed improvement in signal-to-noise ratio. Strong settings can suppress
weak speech. The implementation and browser control have automated tests, and
the current NR has also been confirmed in live operation on an RSP1A receiver.

See [the SpeexDSP bundle notes](external/speexdsp-1.2.1/README.md) for the supplied
library, headers, license and checksum manifest.

## Automatic FT8 reception

A separate USB receiver automatically listens on a standard FT8 frequency when
its full audio passband fits inside the selected band segment. It runs even
without browser listeners and is independent of their tuning, volume and NR.

Complete UTC-aligned 15-second audio slots are sent to the bundled `ft8-decoder`.
Results appear in the terminal and in both browser interfaces. The tables retain
the last 500 received rows locally and follow new rows while scrolled to the
bottom. Reconnecting supplies the current FT8 frequency status and future decodes.
The mobile table scrolls independently below its waterfall.

Rows show UTC, received signal frequency, SNR, time offset, country and message.
The signal frequency includes the USB audio offset. Country names come from the
bundled AD1C CTY/DXCC data (version 20261023), using the transmitting callsign.
Unknown or unresolved callsigns display `—`; no external callsign service is
queried. Country names describe callsign allocations, not a verified location.

The standard dial frequencies in MHz are 1.840, 3.573, 5.357, 7.074, 10.136,
14.074, 18.100, 21.074, 24.915, 28.074, 50.313, 70.154, 144.174 and 432.174.
The currently offered 70 cm segments do not include 432.174 MHz. Segments without
a matching frequency, including CB, PMR and ADS-B, show an inactive FT8 status.

The receiver host's clock must be synchronized to UTC. Short sample gaps are
filled with zero IQ to preserve slot timing, within limits of 5 ms per gap and
20 ms per slot. Larger discontinuities, band changes and clock steps discard the
affected partial slot; reception resumes with the next complete slot. Missing
RF information cannot be recovered by zero filling.

For residual device/driver timing offsets, set `FT8_TIME_OFFSET_MS` before starting
the receiver (default 0, allowed range −5000 to +5000). Positive values assign
samples a later UTC time; negative values compensate extra receive latency.

The decoder can also be built and tested separately:

```sh
make ft8
make ft8-test
./ft8-decoder --output text --utc 133430 ft8/tests/data/210703_133430.wav
```

See [ft8/README.md](ft8/README.md) for standalone input formats, examples, output
protocol and upstream source information. To update country data, obtain a new
[CTY.DAT](https://www.country-files.com/cty/cty.dat), run
`python3 ft8/tools/generate_country_data.py /path/to/cty.dat`, and redeploy the
generated `html/ft8-country-data.js`.

## ADS-B reception

Select **ADS-B** in the desktop band menu to receive at 1090 MHz. The desktop
interface shows an aircraft table and an optional map. The receiver automatically
starts the external `readsb` decoder in ADS-B mode and stops it when leaving that
band. ADS-B uses the shared RF receiver, so normal band-selection restrictions apply.

ADS-B requires a compatible executable at **`./readsb`**, relative to the receiver
process's working directory. It is not built by this project's `make` target.
The decoder must accept UC8 IQ from an input FIFO and support `--device-type ifile`,
`--ifile`, `--iformat UC8`, `--net-json-port` and `--net-json-port-interval`.
A binary copied from another architecture may need to be replaced.

The IQ FIFO is `/tmp/adsb_iq.fifo`; the JSON connection uses `127.0.0.1:30047`.
Decoder output is appended to `readsb.log` in the working directory. For a service
installation, set its working directory so `./readsb` can be found and its log
can be written. The normal FT8/browser-audio features do not require `readsb`.

The map uses Leaflet from a CDN and OpenStreetMap tiles. Adjust `HOME_LAT` and
`HOME_LON` in `html/index.html` for your station before deploying if you want
correct station markers and aircraft distances.

## Tests and diagnostics

These targets run without SDR hardware after installing the build dependencies.
Node.js is needed for the JavaScript tests. The two WebSocket transport tests
require TCP port 9001 to be free.

| Command | Checks |
| --- | --- |
| `make nr-test` | Noise attenuation, speech preservation, exact float bypass, buffering/reset, per-listener AGC/NR, FM bypass and both browser controls |
| `make login-test` | Callsign validation, user counting, band permissions, NR commands, logout and browser connection handling |
| `make ft8-test` | Standalone decoder integration and bundled WSJT-X/reference-file checksums |
| `make ft8-live-test` | Synthetic reception through the production DSP/UTC-slot path and real decoding; no SDR device is used |
| `make ft8-web-test` | Real WebSocket transport, two clients, decode bursts, metadata and existing packet types |
| `make ft8-url-test` | Gateway selection, login/reconnect and both browser connection flows |
| `make ft8-country-test` | Country lookup, transmitting-callsign selection and both FT8 tables |
| `make client-diagnostics-test` | Connection/login diagnostics using real WebSocket clients |

Receiver messages identify browser connections as `[Client #N]` and report logins,
rejections, callsign changes, logout and disconnect statistics. Connections without
valid login data are reported after 15 seconds and then at most every five minutes.
Transport peers and forwarded proxy headers are logged separately. Query strings,
cookies, authorization headers and password fields are omitted. Header values
are logged as supplied by the proxy, with bounded and escaped previews.

FT8 logs report inactive band segments, discarded partial slots and decoder
errors. For a service deployment, consult the receiver's systemd journal together
with `readsb.log` when diagnosing ADS-B.

## License and bundled components

The project is distributed under [GPL-3.0](LICENSE). The bundled FT8 decoder
includes its WSJT-X source notices and [license](ft8/COPYING). SpeexDSP includes
its own [BSD license](external/speexdsp-1.2.1/LICENSE). Country-data attribution and
license text are embedded in `html/ft8-country-data.js`. Retain the corresponding
notices when redistributing these components.

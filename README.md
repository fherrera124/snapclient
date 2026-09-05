# Snapcast client for ESP32

### Synchronous multiroom audio streaming client for [Snapcast](https://github.com/badaix/snapcast)

A C++ rewrite of the ESP32 snapclient. The audio path, the Snapcast
protocol and the sync algorithm live in a portable library that builds and
runs on a desktop; only the I2S output, the timer, WiFi provisioning and NVS
are ESP-IDF specific.

## Feature list

- Snapcast protocol client with server-clock sync (Kalman offset/drift
  filter, sliding-median age control, sample-level correction)
- PCM, Opus and FLAC streams
- Software DSP: stereo, biamp, bass boost, bass/treble EQ
- Web interface and HTTP API for settings, on port 80 by default
- WiFi provisioning over [Improv Serial](https://www.improv-wifi.com/)
- mDNS discovery of the Snapcast server
- Remote logging over UDP
- Optional TAS5805M amplifier control over I2C
- Host build with unit tests, no hardware required

## Layout

```
core/      portable library - protocol, sync, dsp, settings, web UI
main/      ESP-IDF application - I2S sink, timer, WiFi, NVS
external/  bell (submodule)
reference/ ESP-ADF components kept for reference, not built
```

`core/` never includes an ESP-IDF header. It talks to the platform through
small interfaces the application implements — `AudioSink` for output,
`PrecisionWaiter` for the wait a lock needs, `SettingsStore` for
persistence. That is what lets the same sync engine run under a host test.

### bell

[bell](https://github.com/fherrera124/bell) is vendored as a submodule and
does most of the heavy lifting that would otherwise be ESP-IDF calls: tasks,
sockets, an HTTP server and client, mDNS, the logger, the audio codecs and
the DSP primitives. It presents them as ordinary C++ classes with the same
API on the device and on a desktop.

That is the reason this port is not tied to Espressif's framework. Writing
against `bell::Task` and `bell::net::TCPSocket` instead of `xTaskCreate` and
`lwip` keeps the interesting code — sync, buffering, decoding — free of
`#ifdef ESP_PLATFORM`, testable off-device, and portable to another target
if bell grows one. The few places that genuinely need the SDK are confined
to `main/`.

What a snapclient does not need is switched off at configure time
(`BELL_DISABLE_MQTT`, `BELL_CODEC_AAC` and friends in the top-level
`CMakeLists.txt`), so none of it reaches the binary.

## Hardware

Tested on a plain ESP32 (ESP32-D0WDQ6) with 4MB flash and an I2S DAC. No
PSRAM required. Pin assignments are `idf.py menuconfig` options under
**Snapclient Configuration**, so a different board is a config change rather
than a code change.

## Build and flash

Requires ESP-IDF v5.x or v6.x (developed against v6.0.1).

```
git clone --recurse-submodules <this repo>
cd snapclient
```

If you already cloned without submodules:

```
git submodule update --init --recursive
```

Set the target and configure:

```
idf.py set-target esp32
idf.py menuconfig
```

Under **Snapclient Configuration**, set the I2S pins for your DAC and the
Snapserver host and port. There is no WiFi option there on purpose - see
[WiFi provisioning](#wifi-provisioning) below.

```
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The partition table is a single 2MB factory app — over-the-air update is not
implemented yet; see `docs/ota-plan.md`.

## Ethernet

Off by default. Turn it on under **Ethernet Configuration** — `Internal EMAC`
for a board with a PHY on the RMII pins, `SPI Ethernet` for a W5500 and
friends. Both come from [espressif/ethernet_init][ethernet-init], which
carries the chip, PHY and GPIO options.

[ethernet-init]: https://components.espressif.com/components/espressif/ethernet_init

Ethernet and WiFi both stay up. Whichever has an address is used, and the
wired link takes the default route while it is connected (`route_prio` 150
against WIFI_STA's 100).

All three Ethernet pinouts this port inherited are in `boards/` — see below.

**The Olimex ESP32-PoE clocks its PHY from the ESP32**, and ESP-IDF warns
that RMII CLK output is unstable on the ESP32 when Ethernet and WiFi run
together. If that board misbehaves with both up, that erratum is the first
thing to suspect.

None of the Ethernet paths have run on hardware — there was no Ethernet
board to test with. They are compile-verified only.

## Boards

`boards/` holds one defaults fragment per board, carrying its I2S pins and,
where it applies, the DAC's I2C pins and the Ethernet PHY wiring. Layer one
on top of `sdkconfig.defaults`:

```
rm sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/tas5805m.defaults" build
```

Defaults only seed a fresh config, hence the `rm` — an existing `sdkconfig`
keeps whatever it already recorded.

| File | MCLK | BCLK | WS | DOUT | Extra |
|---|---|---|---|---|---|
| `pcm5102a` | 0 | 26 | 25 | 22 | — |
| `tas5805m` | 0 | 26 | 25 | 22 | I2C 21/27, addr 0x2D |
| `olimex-esp32-poe` | 0 | 13 | 14 | 32 | LAN87xx, RMII clock out |
| `wt32-eth01` | -1 | 15 | 4 | 14 | LAN87xx, RMII clock in |
| `adau1961-custom` | 3 | 15 | 13 | 4 | LAN87xx, RMII clock in |
| `max98357a-esp32s2` | -1 | 10 | 11 | 12 | esp32s2 target |

These pinouts came from the `sdkconfig_*` dumps this port inherited, which
have been removed: they were full configs for a different application on
ESP-IDF 5, so most of what they contained no longer resolves, and copying
one over `sdkconfig` silently dropped the settings that mattered. All six
fragments are build-verified.

The ESP32-S2 build works but has never been run. That part is single-core,
so playback pacing and lwIP share one core rather than having one each,
which is the arrangement `SnapclientTask`'s priority was chosen for — see
the comment on it in `main/main.cpp`. Whether one core keeps up with Opus
or FLAC decoding plus sync is untested. The parts that made it fail to
compile at all are fixed: the IRAM chunk tier is now behind
`CONFIG_HEAP_HAS_EXEC_HEAP`, since instruction and data RAM are one region
there, and the task falls back to no core affinity under
`CONFIG_FREERTOS_UNICORE`.

## Host build and tests

`core/` builds on its own, so the protocol, sync and pool logic can be
tested without a board:

```
cmake -S core -B build-host -DSNAPCLIENT_CORE_BUILD_TESTS=ON
cmake --build build-host
./build-host/tests/sync_engine_test
./build-host/tests/chunk_pool_test
```

`sync_engine_test` drives `SyncEngine` through controlled `age` sequences;
`chunk_pool_test` covers the decoded-chunk pool and pins `DynamicResampler`
bit-for-bit against a reference implementation.

## Test against a server

Set up a Snapcast server on your network. On a Linux box, install
[Snapcast](https://github.com/badaix/snapcast) and start it (skip if you
installed it as a service):

```
snapserver
```

Pipe some audio into its fifo:

```
mplayer http://ice1.somafm.com/secretagent-128-aac -ao pcm:file=/tmp/snapfifo -af format=s16LE -srate 48000
```

If the client sounds early or late against another client, that is normally
the per-client `latency` setting in the server rather than a bug — different
output paths genuinely have different lengths, and that setting is what
reconciles them.

### Buffer size and memory

The server's `buffer` setting decides how much audio the client must hold,
and the client's queue is sized from it. FLAC payload size depends on how
well the material compresses, so dense music needs considerably more memory
than speech for the same buffer. On a 4MB ESP32 without PSRAM, 500ms is
comfortable; larger values can run the heap out on demanding material.

The client says so when it happens rather than dropping audio quietly — see
below.

## Settings and the web interface

Everything except the pin assignments is configurable at runtime and stored
in NVS. Point a browser at the client's address for the settings pages, or
use the API directly:

```
curl -s http://<client-ip>/api/settings
curl -X POST http://<client-ip>/api/settings -d '{"server":{"host":"192.168.1.10"}}'
```

`POST /api/settings` merges — fields you leave out keep their stored value.
Changing the server host or the hostname needs a restart (`POST
/api/restart`); DSP and logging settings apply immediately.

`GET /api/discover` browses mDNS for Snapcast servers on the network.

With a TAS5805M wired to the I2C pins configured in menuconfig,
`/api/dac/settings` and `/api/dac/faults` are registered as well, and the
web interface grows an amplifier page.

## Diagnosing dropouts

Audio can be lost in a few places, and each one says so in the log,
rate-limited, with what was lost, how much has gone, and which side is
behind:

```
audio lost: no memory for a 3184 byte chunk (47 chunks so far, heapFree=8420) - lower the server's buffer setting
audio lost: chunk queue full at 30 (12 chunks so far, decode is behind)
audio starved: nothing decoded for 10ms (47 times, 0 chunks waiting) - the network is behind
chunk pool missed for 4608 bytes (3 total, 6 slots)
```

The first three tell you whether to reach for the server's buffer setting,
the decoder, or the network. The last one means the chunk pool is mis-sized
for the pipeline, which is a bug worth reporting.

`hard resync` lines are a different thing: the playback clock drifted past
its threshold and the client re-locked, which is audible as a break.

## Remote logging over UDP

Every log line the firmware produces can also be sent to another machine on
the network as a plain-text UDP datagram — one datagram per line, no ANSI
colors, no buffering and no retry. It is meant for watching a board that is
running normally, without a USB cable and without holding the serial port
(which `idf.py flash` needs back anyway).

It is a `bell::LoggerBackend` registered alongside the serial one, so it
carries exactly the same lines at the same levels, not a separate feed. The
target is stored in NVS and applied the moment you save it — no rebuild and
no restart. It is off by default; the default port is 9999.

Because it is plain UDP, datagrams can be dropped or arrive out of order,
most likely when the network is struggling, which is often exactly when you
are reading. Treat a jump in a cumulative counter as a lost datagram rather
than a bug. It also shares the WiFi with the audio stream, so a burst of
warnings is not free.

### Listening on Linux

`socat` is the more reliable of the two, since `nc` in UDP mode latches onto
the first sender and some builds stop printing after a pause:

```
socat -u UDP-RECV:9999 -
```

```
nc -ul 9999
```

### Pointing the board at it

Find the address the board should send to:

```
ip -4 -o addr show scope global
```

Then open the client's web interface, go to **Device** → **Remote logging**,
tick *Send logs over UDP*, fill in that address and port 9999, and save.

The same thing over the API, if you prefer:

```
curl -X POST http://<client-ip>/api/settings \
  -d '{"logging":{"enabled":true,"udpHost":"192.168.1.50","udpPort":9999}}'
```

Both write the same three settings, and either way the lines start arriving
immediately.

## WiFi provisioning

Credentials are not built into the firmware. They are sent to a running
board over the serial port with [Improv Serial](https://www.improv-wifi.com/):
open the page with the board connected over USB and hand it the network and
password. `esp_wifi` stores them in flash itself and reuses them on every
later boot and reconnect, so this is a one-time step per board.

## Credits

Forked from [CarlosDerSeher/snapclient](https://github.com/CarlosDerSeher/snapclient),
whose C implementation this port follows for the sync algorithm in
particular, and which remains the reference for behaviour on this hardware.
That in turn builds on
[jorgenkraghjakobsen/snapclient](https://github.com/jorgenkraghjakobsen/snapclient).

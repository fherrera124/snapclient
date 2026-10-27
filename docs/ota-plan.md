# OTA firmware updates - plan

Not started. Planned as an **opt-in development utility**, not a product
feature.

## What this is, and what it is not

OTA here exists so that a board on a shelf can be reflashed without a cable
during development - long soak runs and reconnect testing are the actual
motivation. It is deliberately **not** a supported update channel: no
authentication, no image signing, no version tracking, no rollout control.

That framing is what settles the design. The objections that make a bare
LAN OTA endpoint a bad idea all attach to a feature with a support promise
behind it; none attach to a tool the developer opts into on their own bench.
The build-time gate *is* the security model.

Consequences:

- Compiled out by default behind `SNAPCLIENT_ENABLE_OTA`.
- Documented in the README as a development convenience, with the trust
  assumption stated plainly.
- Not advertised as a feature of the port.

## Gate the code, not the flash layout

`SNAPCLIENT_ENABLE_OTA` covers application code only. **`partitions.csv`
moves to the OTA layout unconditionally**, whether or not the feature is
compiled in.

Making the table conditional too would mean `erase_flash` plus
re-provisioning (WiFi credentials, DAC/EQ settings, hostname) on every
toggle of the option - which destroys the convenience the feature exists
for. Unconditional means toggling is a plain rebuild and reflash.

It also lands the one piece that can never be delivered remotely: the
partition table lives at 0x8000 and `esp_ota_*` only rewrites an app
partition, so this specific change always requires a cable. Doing it once,
now, keeps the door open regardless of what happens to the rest of this
plan.

Price when the feature is compiled out: a per-image ceiling of 1984K
instead of 2M, and one `erase_flash`.

## Design

### Partition table - `partitions.csv`

```
# Name,     Type, SubType, Offset,   Size, Flags
nvs,        data, nvs,     0x9000,   0x6000,
otadata,    data, ota,     0xf000,   0x2000,
phy_init,   data, phy,     0x11000,  0x1000,
ota_0,      app,  ota_0,   0x20000,  0x1F0000,
ota_1,      app,  ota_1,   0x210000, 0x1F0000,
```

`nvs` keeps its current offset and size. `phy_init` **moves** from 0xf000 to
0x11000 to make room for the mandatory 8K `otadata`. Each OTA slot gets
1984K - the same per-slot size the old app shipped on this same hardware,
and exactly the space currently sitting unused behind the 2M `factory`
partition. Fits 4MB flash exactly. The gap between `phy_init` (ends
0x12000) and `ota_0` (0x20000) is the 64K alignment app partitions require,
not slack.

Current binary is 1.68MB (1,762,224 bytes), leaving ~263KB of headroom per
slot. Worth re-checking before adding another codec.

Real-device consequence: changing partition offsets means a device flashed
with the old table needs a full erase before the first flash with this one
(the same erase step already used earlier in this project), which also
clears NVS. Call this out before flashing hardware rather than working
around it in code.

### `SNAPCLIENT_ENABLE_OTA` - `main/Kconfig.projbuild`

```
    config SNAPCLIENT_ENABLE_OTA
        bool "Enable OTA firmware updates (development utility)"
        default n
        help
            Adds POST /api/ota, which replaces the running firmware with
            an uploaded image. There is no authentication: anyone who can
            reach the web port on the network can reflash the device.
            Intended for development on a trusted LAN.
```

Note the project's other LAN development utility, UDP logging, is gated at
*runtime* instead (`settings.udpLogEnabled()`, `main/main.cpp:79` - NVS plus
the settings UI). OTA deliberately differs: an idle log sink is harmless
whether or not it is armed, a firmware-replacement route is not, and "not
compiled in" is a stronger guarantee than a flag that says off.

### `OtaTarget` - new, `core/include/snapclient/OtaTarget.h`

Abstract interface, portable, no ESP-IDF headers - same role `I2cBus.h`
plays for the DAC work:

```cpp
class OtaTarget {
 public:
  virtual ~OtaTarget() = default;
  virtual bool begin(size_t imageSize) = 0;
  virtual bool write(const uint8_t* data, size_t len) = 0;
  virtual bool end() = 0;              // finalize + validate the image
  virtual bool setBootPartition() = 0;
  virtual void reboot() = 0;
};
```

### `OtaUpdater` - new, `core/include/snapclient/OtaUpdater.h` / `core/src/OtaUpdater.cpp`

Decision-free logic layer over `OtaTarget`, testable with a plain
`std::istringstream` - no fake socket needed:

```cpp
class OtaUpdater {
 public:
  explicit OtaUpdater(OtaTarget& target);
  // Returns bytes read, 0 at end of body.
  using BodySource = std::function<size_t(std::byte* dst, size_t len)>;
  // Streams exactly contentLength bytes from readChunk into the target and
  // validates the image. Leaves the current boot partition untouched on
  // any failure.
  bool apply(const BodySource& readChunk, size_t contentLength,
             std::string& error);
  void reboot();  // passthrough - caller decides when to call this
 private:
  OtaTarget& target_;
};
```

`apply()` does `begin(contentLength)` -> read/write loop in fixed 4KB
chunks (failing cleanly on a short read) -> `end()` -> `setBootPartition()`,
returning false with a message in `error` at the first failed step and never
calling `setBootPartition()` if `end()` (image validation) failed. It does
**not** call `reboot()` itself - the caller writes the HTTP response first,
then calls `reboot()` separately, so the acknowledgment reflects the real
outcome.

### Reading a firmware-sized body - done, in bell

`getBodyBytes()` / `getBodyStringView()` fully buffer the body into a
`std::vector<char>`, which is wrong for a firmware-sized upload, and reading
the socket directly via `getStream()` is worse: `Server::readFromClient()`
drains whatever the handler left behind *after* the handler returns, and did
so by calling `getBodyBytesLength()`. With the body already off the stream,
that resized the shared buffer to Content-Length - a ~1.7MB allocation - and
then blocked on bytes that were never coming. Invisible on the happy path,
since `reboot()` restarts inside the handler; live on exactly the failure
path this design promises to survive.

`Reader` therefore grew a streaming pair: `readBodyChunk(dst, len)`, which
reads into a caller-supplied buffer and advances the consumed count, and
`discardRemainingBody()`, which the server now calls instead. A fully
consumed body makes the drain a no-op; a half-consumed one is drained for
real up to `maxDrainLen` (64K) and otherwise closes the connection. The
route handler passes `[&](std::byte* d, size_t n) { return
reader->readBodyChunk(d, n).value_or(0); }` as the `BodySource`.

### `Esp32OtaTarget` - new, `main/Esp32OtaTarget.h` / `.cpp`

`begin()` calls `esp_ota_get_next_update_partition(nullptr)` plus
`esp_ota_begin()`; `write()` / `end()` / `setBootPartition()` map directly
to `esp_ota_write()` / `esp_ota_end()` / `esp_ota_set_boot_partition()`;
`reboot()` does `vTaskDelay(pdMS_TO_TICKS(2000))` then `esp_restart()`. The
delay gives the already-written HTTP response a chance to leave the socket
before `esp_restart()` tears down the network stack; it stays in this
ESP32-only file rather than in the portable `OtaUpdater`, same reasoning as
keeping the TAS5805M byte-swap quirk inside its own driver.

Every `esp_ota_*` return value is checked and converted to a `false` return.
No `ESP_ERROR_CHECK` anywhere in this path.

### `ControlServer::registerOtaRoute(OtaUpdater&)`

Opt-in method, same shape as `registerDacRoutes`, called from `main.cpp`
under `#ifdef CONFIG_SNAPCLIENT_ENABLE_OTA`. Registers two routes:

- `POST /api/ota` - reads `request->getContentLength()`, calls
  `otaUpdater.apply(*request->getStream(), ...)`, writes a small JSON status
  (200 on success, 500 with `{"error": "..."}` on failure), and only on
  success calls `otaUpdater.reboot()` **after** `writeResponseWithBody()`
  returns.
- `GET /api/ota` - returns `{"available":true}`. Exists so the UI can probe
  for the feature the same way it probes `/api/dac/settings`; a POST-only
  route cannot be probed.

### Boot-time rollback confirm - `main/main.cpp`

`esp_ota_mark_app_valid_cancel_rollback()` called once, early in
`app_main()` right after `nvs_flash_init()` succeeds, and
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` added to `sdkconfig.defaults`.

Together these mean an image that never gets far enough to reach that line
(a crash loop before WiFi comes up) is auto-reverted by the bootloader on
the next reset instead of bricking the device. Note the limit honestly: an
image that boots fine but cannot play audio marks itself valid and stays.
Proving playback health before marking valid is a larger design than this
slice warrants - but do not describe the rollback as more than it is.

### Web UI - `core/src/SettingsUiHtml.cpp`

New "Firmware" tab, following the `dacTab` pattern already in
`kNavShellHtml` exactly: the nav entry ships `style="display:none"` and is
revealed by a probe, here `fetch('/api/ota')`, so a build with the feature
compiled out shows no dead tab.

The page itself is `<input type="file">` plus an upload button and a status
line, using the existing `<p id="...Status">` convention.
`fetch('/api/ota', { method: 'POST', body: file })` posts the raw file bytes
with `Content-Length` set automatically by the browser - no
FormData/multipart, since the server reads a raw body. The catch block
treats a network error as "upload finished, device is rebooting" rather than
a hard failure, since the connection can drop the instant `esp_restart()`
runs.

### README

A short section stating: development convenience rather than a supported
update channel; no authentication, so anyone who can reach the web port can
reflash the device, and it should only be enabled on a trusted network; off
by default via `SNAPCLIENT_ENABLE_OTA`; playback stops while an update runs.

## Do not repeat the old app's mistakes

`reference/ota_server/ota_server.c` is the old implementation. It works
end-to-end on the happy path and falls apart everywhere else - each of
these is a thing the design above avoids, not a thing to port:

- It tears down the player at `accept()`, **before receiving a byte**
  (`vTaskDelete(t_http_get_task)` plus `deinit_player()`), so any TCP
  connection to the port kills audio. Going through `bell::http::Server`
  means nothing is touched until a real body is arriving.
- A connection that sends nothing exits the receive loop having never
  called `esp_ota_begin()`, then calls `esp_ota_end()` on an
  **uninitialized stack handle**.
- An HTTP request with no `Content-Length` header NULL-derefs: it does
  `strstr(buf, "Content-Length: ") + strlen(...)` with no check and hands
  the result to `sscanf`. Pointing a browser at the port bounces the board.
- It sends `200 OK` **before** `esp_ota_end()` validates the image. It does
  not brick, but only because `setBootPartition()` happens to come after
  validation - the safety is in the call order, not in error handling.
- `ESP_ERROR_CHECK` wraps `esp_ota_begin` and `esp_ota_end`, so an invalid
  image panics rather than failing cleanly.
- `esp_ota_mark_app_valid_cancel_rollback()` fires at task start, before the
  image has proven anything. The author left a TODO admitting it.
- No web UI at all: raw TCP on port 8032 plus `curl`.

## Verification

- **`Core::otaUpdaterSmokeTest()`** (new, mirroring the `RecordingI2cBus`
  pattern used for the DAC driver): a `RecordingOtaTarget` fake records
  written bytes and call order, and can be configured to fail at a chosen
  step.
  - Happy path: `apply()` over an in-memory `std::istringstream` "image" ->
    byte-exact match, correct call order, `setBootPartition` called.
  - Validation-failure path: force `end()` to fail -> assert
    `setBootPartition` and `reboot` are never reached. This is the core
    safety property the ack-ordering exists for.
  - Truncated-body path: `contentLength` claims more than the stream
    yields -> clean failure, no partial-success response.
- **Web UI**: headless-Chrome screenshot of the new tab and page.
- **ESP32 target**: compile-only, in both `SNAPCLIENT_ENABLE_OTA` states.
  The actual `esp_ota_write()`-to-flash path, the reboot into the new
  partition, and a real rollback trigger all need the user's device.
- Comment-norm re-check immediately before commit.

## Out of scope

- `esp_https_ota` / pull-from-URL updates - mbedTLS is disabled
  project-wide (`CMakeLists.txt`: `set(BELL_DISABLE_MBEDTLS ON ...)`), which
  is also why this keeps the push-upload model.
- Auth or a token on `/api/ota` - the build-time gate is the trust
  boundary, per the framing at the top.
- Image signing and secure boot - a product concern, not a bench tool's.
- Upload progress percentage in the UI.
- Firmware version tracking or display - no version metadata infrastructure
  exists anywhere in this codebase.
- General `ControlSettings` / DAC route wiring - already done, see
  `main/main.cpp:70-102`.

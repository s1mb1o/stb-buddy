# Change log

## 0.12.3 — 2026-10-08

- Moved MCP execution to two bounded asynchronous request workers so long UART
  waits and file downloads do not block the WebUI or REST API.
- Aligned `lircd.conf` frequency, duty-cycle and gap validation with the RMT
  transmitter bounds; added hostile boundary tests.
- Marked arbitrary UART writes and physical IR actions as potentially
  destructive in MCP tool annotations.
- Added self-contained SPACE_ENC and RC5 fixtures plus GitHub Actions host and
  firmware builds.
- Enabled ESP-IDF reproducible builds so release binaries omit build timestamps
  and local source paths.
- Added the public security policy and removed device-, network- and
  organization-specific data from the public documentation and Git history.

- Replaced the README with a GitHub-ready operating guide in STE-style English.
- Added seven original AtomS3R device drawings in PNG and SVG formats.
- Added a drawing generator with example network values and valid QR payloads.
- Documented Wi-Fi setup, wiring, button actions, browser controls, REST, and MCP.
- Documented file transfer, firmware updates, build steps, and security limits.
- Added the MIT project license. Preserved third-party license notices.

## 0.12.2 — 2026-10-08

- Added a bounded display-only event channel for successful UART writes made
  through MCP and the public HTTP API.
- Rendered MCP commands, immediate MCP replies and direct HTTP writes as bold
  magenta `[MCP]`, `[MCP reply]` and `[HTTP]` browser annotations.
- Kept browser keyboard input unannotated and kept every annotation out of raw
  UART history, offsets, downloads, reads and pattern matching.
- Extended **Clear History** to remove the UART-write annotation ring.

## 0.12.1 — 2026-10-08

- Reordered the browser toolbar actions to **IR**, **Clear History**,
  **Download file…**, **Download log**, **OTA update**, and **Help**.
- Shortened the retained-history download label from **Download log file** to
  **Download log**.

## 0.12.0 — 2026-10-08

- Added an anchored **Help** popup with UART wiring, console, file-transfer,
  IR, Wi-Fi recovery, OTA and security instructions.
- Added a self-contained OpenAPI 3.1 document at `/openapi.json` covering the
  public REST API and generic Streamable HTTP MCP transport.
- Added a dependency-free, read-only API reference at `/docs`; it renders the
  embedded schema without a CDN or dangerous “try it” controls.

## 0.11.1 — 2026-10-08

- Increased the bounded HTTP client-session limit from four to eight and
  enabled least-recently-used session purging. This prevents a browser's
  parallel keep-alive connections from starving WebUI and API requests.

## 0.11.0 — 2026-10-08

- Added a wide anchored **IR** popover to the browser toolbar with remote
  selection, every named key and an individual **Send** action.
- Added browser upload and exact persisted-file download for `lircd.conf`.
- Added `GET /api/remotes/config` for bounded download of the active persisted
  raw configuration without reconstructing it from parsed data.

## 0.10.0 — 2026-10-08

- Made the `UART1 · <baud>` status item open a compact, anchored port
  properties panel for baud rate, data bits, parity and stop bits.
- Added a bounded runtime UART configuration REST endpoint shared with the
  existing atomic UART configuration path. Changes remain non-persistent and
  reboot restores the firmware defaults.

## 0.9.0 — 2026-10-08

- Added serial-hub-style **Download file…** shell validation, source and local
  SHA-256 checks, 16 KiB verified serial chunks, `nc` acceleration/fallback,
  bounded retry handling and automatic browser saving.
- Run browser downloads in a background task so port 80 and UART RX remain
  responsive, with one verified file up to 2 MiB cached only in PSRAM.
- Added asynchronous REST job/status/file endpoints and a synchronous MCP
  `download` tool returning metadata and the browser download URL.

## 0.8.0 — 2026-10-08

- Consolidated the setup page, browser console, REST APIs, OTA and MCP onto a
  single HTTP server on port 80; port 8765 is no longer used.
- Added the running firmware version and OTA file upload/progress/restart
  controls to the browser console, alongside retained-log file download.
- Added a bounded in-memory IR event stream for successful named-key, NEC and
  raw transmissions initiated through either HTTP or MCP.
- Show IR events as bold magenta annotations in the browser terminal without
  contaminating byte-exact UART history, offsets, downloads or MCP reads.
- Detect and report browser-side gaps if more than 32 IR events arrive before
  the event stream is polled.

## 0.7.0 — 2026-10-08

- Added a native stateless Streamable HTTP MCP endpoint at `/mcp` with both
  handshake-era initialization and MCP 2026-07-28 discovery.
- Added 12 bounded MCP tools for device status, exact UART history reads,
  serialized writes and waits, history clearing, runtime UART configuration,
  persistent `lircd.conf` loading, remote discovery, and named/NEC/raw IR.
- Return both UTF-8-safe UART text and Base64 exact bytes with monotonic offsets.
- Added explicit tool input/output schemas, structured results and accurate
  read-only/destructive/idempotent annotations.
- Added runtime UART baud, data-bit, parity and stop-bit configuration without
  interrupting the independent UART receive task.
- Added standard and extended-address NEC generation plus bounded raw RMT
  transmission through the existing serialized IR channel.
- Verified the endpoint with MCP Python 2.3.0, Codex CLI 0.161.0 and OpenCode
  2.0.24, and fixed a response-header lifetime defect found by the Codex test.

## 0.6.0 — 2026-10-08

- Added a station-mode UART wiring screen selected with a short button press.
- Showed the common ground and crossed TXD/RXD connections with AtomS3R GPIO1
  and GPIO2 identifiers.
- Added 3.3 V UART and USB-powered 5 V-disconnection instructions to the
  screen.

## 0.5.0 — 2026-10-08

- Moved the setup form and Wi-Fi provisioning API to HTTP port 80 while
  retaining the normal browser console and full API on port 8765.
- Added short-button display navigation in setup mode: main status, open-AP
  Wi-Fi QR code, setup-page URL QR code, then back to main.
- Kept the two-second setup gesture independent, so releasing after a long
  press does not advance the display screen.
- Restored left text alignment before the `IP ADDRESS` label so short SSIDs do
  not leave the label centered off-screen.

## 0.4.0 — 2026-10-08

- Added station-mode Wi-Fi credentials in NVS and a recovery setup flow: a
  two-second button hold starts the open `Buddy-<MAC>` AP and serves a bounded
  SSID/password form at `192.168.4.1:8765`.
- Added the AtomS3R 128 x 128 status display with setup/connecting/online
  states, a horizontally scrolling SSID, IPv4 address, and long-press progress.
- Added an embedded xterm.js web console patterned after `tools/serial-hub`,
  including bounded live history reads, serialized keyboard-to-UART writes,
  automatic light/dark theme, retained-log download, and complete history
  clearing.
- Pinned M5GFX 0.2.32 and embedded all browser assets so the device has no CDN
  or other runtime dependency.
- Flashed an AtomS3R and verified the complete first-boot path through setup AP,
  GC9107 display detection, UART, IR initialization and HTTP readiness.

## 0.3.0 — 2026-10-08

- Added streamed application OTA uploads to the inactive flash slot through
  `POST /api/ota`.
- Added OTA version and partition status through `GET /api/ota`, plus an
  explicit delayed restart endpoint at `POST /api/ota/reboot`.
- Validate the ESP application image and `stb_buddy` project name before
  changing the selected boot slot.
- Enabled bootloader rollback and confirm a newly installed image only after
  all runtime services initialize successfully.

## 0.2.0 — 2026-10-08

- Added atomic `lircd.conf` upload and persistent NVS restore through
  `PUT /api/remotes`.
- Added `GET /api/remotes` discovery with remote indexes and key names.
- Added named-key transmission through `POST /api/ir/send`.
- Implemented a bounded `SPACE_ENC` and `RC5` subset, including LIRC-compatible
  gaps, repeat frames and toggle bits.
- Added host tests that validate parser and waveform generation without
  transmitting IR.

## 0.1.0 — 2026-10-08

- Created the standalone `stb-buddy` ESP-IDF project for M5Stack AtomS3R.
- Added a bounded PSRAM UART history, UART1 configuration for the HY2.0 G1/G2
  connector, an initial Wi-Fi setup access point, an HTTP health endpoint, and
  RMT initialization for the onboard GPIO47 IR LED.
- Documented the intended MCP and IR tool surface. MCP request handling and IR
  transmission commands are not implemented in this bootstrap revision.
- Verified version 0.1.0 builds for ESP32-S3 with ESP-IDF v5.5.5.

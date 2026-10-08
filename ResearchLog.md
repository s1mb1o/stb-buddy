# Research log

This file records design decisions that are useful to contributors. Device
addresses, network names, private fixture paths and customer hardware details
are deliberately excluded.

## 2026-10-08 — Asynchronous MCP requests

- ESP-IDF's HTTP server dispatches normal handlers on one server task. Waiting
  for UART output or completing a file transfer there would prevent the WebUI
  and REST API from responding.
- MCP requests therefore use two bounded asynchronous request workers. The
  server task hands off the request and immediately resumes HTTP dispatch.
- A third simultaneous MCP request receives HTTP 503 with `Retry-After: 1`.
  This bounds memory use and makes overload visible to clients.
- UART transaction locking still serializes operations that must not interleave.

## 2026-10-08 — UART history and presentation annotations

- The PSRAM ring stores raw UART RX bytes. Its absolute offsets never include
  browser-only status text.
- Successful public HTTP writes and MCP commands use a separate 32-record
  annotation ring. Successful IR transmissions use another 32-record ring.
- Annotation text is escaped and bounded. These records cannot satisfy a UART
  wait pattern and do not appear in log downloads or MCP reads.
- Clearing history also clears both presentation rings, so cleared content
  cannot reappear through the browser or MCP.

## 2026-10-08 — LIRC parsing and IR transmission

- The parser implements bounded SPACE_ENC and RC5 subsets rather than every
  LIRC format.
- Atomic configuration replacement keeps the previous remote database active
  if a new upload fails validation.
- Frequency is limited to 20–60 kHz, duty cycle to 1–99 percent, gap to
  1–1,000,000 microseconds, repeat count to 0–10, and generated waveforms to
  the fixed RMT capacity.
- A receiver may synthesize release events; the transmitter sends only the
  configured initial and repeat waveforms.

## 2026-10-08 — Embedded file download

- The firmware verifies a live shell, obtains source size and SHA-256, then
  transfers bounded chunks over UART or accepts a faster network callback.
- Every completed file is checked against the source hash. Failed or partial
  files are not exposed for download.
- One transfer may run at a time. At most one completed 2 MiB file is retained
  in PSRAM, and file contents are never written to NVS or application flash.
- Browser transfers run in their own task. MCP waits in an asynchronous HTTP
  worker so status polling remains available during a long operation.

## 2026-10-08 — HTTP and browser design

- Provisioning, WebUI, REST, OTA and MCP share port 80. A single origin keeps
  browser behavior simple and avoids a second server's sockets and task stack.
- Eight HTTP client sessions plus least-recently-used purging accommodate
  browser parallel connections without exhausting the configured socket pool.
- The embedded OpenAPI page is descriptive rather than interactive because
  several operations affect attached hardware or persistent settings.
- UART, IR and Help panels are anchored non-modal popovers. Escape and an
  outside click close them, and opening one closes the others.

## 2026-10-08 — Display and provisioning

- Setup mode uses an open `Buddy-<MAC>` AP and serves the configuration page at
  `http://192.168.4.1/`.
- Short button presses cycle through status and QR pages in setup mode, and
  through status and UART wiring pages in station mode.
- A two-second hold enters Wi-Fi setup without deleting retained UART history
  or the stored remote configuration.
- The normal screen restores top-left text alignment before drawing the address
  label; otherwise a preceding centered short SSID can clip that label.

## 2026-10-08 — Security boundary

- The HTTP API and setup AP are intentionally unauthenticated and intended for
  trusted networks with controlled physical access.
- Wi-Fi credentials and remote configuration are stored in NVS. Flash
  encryption and secure boot are not enabled by the project configuration.
- OTA validates ESP image structure, project identity, version and partition
  size, but does not add a project-level cryptographic signature.
- See [SECURITY.md](SECURITY.md) for deployment limits and private reporting.

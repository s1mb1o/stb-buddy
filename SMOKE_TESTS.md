# Smoke tests

## README and drawing checks

Run these checks after a documentation change. They do not require an STB connection.

1. Verify that every relative README link and image path exists.
2. Verify that the contents links match the README headings.
3. Compare the REST table with `main/web/openapi.json`.
4. Compare the MCP table with `kToolsJson` in `main/mcp_service.cpp`.
5. Check shell example syntax without executing the examples.
6. Parse all seven SVG files as XML. Verify that all seven PNG files are 1200 × 1200.
7. Decode `ap-wifi-qr.png`. Expect `WIFI:T:nopass;S:Buddy-AABBCCDDEEFF;;`.
8. Decode `ap-setup-url.png`. Expect `http://192.168.4.1/`.
9. Inspect the drawings. Verify that each image shows only the device and its screen.
10. Verify that `LICENSE` contains the project MIT notice. Preserve vendor notices.

The documentation checks passed on 2026-10-08.
This result does not replace physical-device smoke tests.

## Physical device checks

Run these checks on an AtomS3R after a firmware update. Do not connect UART or
transmit IR until the basic display and network checks pass.

## Setup display and HTTP

1. Boot without stored Wi-Fi credentials.
2. Verify that the main screen shows `WIFI SETUP`, the full `Buddy-<MAC>` SSID,
   `192.168.4.1`, and an `IP ADDRESS` label that is fully visible.
3. Join the open setup AP and open `http://192.168.4.1/` without specifying a
   port.
4. Verify that the Wi-Fi form loads its CSS and JavaScript from port 80.
5. Verify that `/health`, `/api/ota`, and `/mcp` are also reachable on port 80.

## Button and QR screens

1. Short-press once and scan the Wi-Fi QR code. Verify that it selects the
   displayed open `Buddy-<MAC>` network.
2. Short-press again and scan the URL QR code. Verify that it opens
   `http://192.168.4.1/`.
3. Short-press again and verify that the main setup screen returns.
4. Hold the button for two seconds and release it. Verify that setup mode starts
   and the release does not advance to a QR screen.

## Station mode

1. Save valid station credentials through the port-80 setup form.
2. Verify that the device restarts, joins the station network, and shows the
   assigned IPv4 address.
3. Open `http://<displayed-ip>/` and verify the browser UART console is served
   directly on port 80 without a redirect or explicit port.
4. Verify that the status bar shows the same running version as `/api/ota`.
5. Select a valid `stb_buddy.bin` with **OTA update**, verify upload progress,
   the validated target version and the separate **Restart** action. Do not
   restart until that version and target partition are expected.
6. Select **Download log** and verify the downloaded bytes match the
   retained UART history and contain no browser-only IR or UART-write
   annotations.

## UART wiring screen

1. Connect the AtomS3R to station Wi-Fi and wait until the display shows
   `ONLINE`.
2. Short-press the button and verify that the UART wiring screen appears.
3. Verify that the screen shows `GND <--> GND`, `G2 TXD --> RXD`, and
   `G1 RXD <-- TXD`.
4. Verify that the screen identifies the UART as 3.3 V and says to leave 5 V
   open when USB-powered.
5. Short-press again and verify that the main status screen returns.
6. Verify that the wiring screen is not shown while station Wi-Fi is still
   connecting.

## Browser UART properties

1. Select `UART1 · 115200` in the status bar. Verify a compact panel opens
   directly below it and shows baud rate `115200`, 8 data bits, no parity and
   1 stop bit from `/health`.
2. Select **Cancel**, reopen the panel and press Escape, then reopen it and
   click outside. Verify all three actions close it without changing the port.
3. Apply the same `115200 8N1` values. Verify the panel closes, the status bar
   remains `UART1 · 115200`, a success message contains `115200 8N1`, and
   `/health` reports the same values.
4. Send an invalid configuration directly to `POST /api/uart/configure` and
   verify HTTP 400 or 422 without changing `/health`.
5. Only with a disposable UART target, apply another supported format and
   verify the header and `/health` update together. Reboot and verify the
   compiled default is restored.

## Browser IR panel

1. Select **IR** in the toolbar. Verify a wide panel opens below the button,
   the UART-properties panel closes, and the loaded remotes appear in a
   selector.
2. Select each remote and verify all of its key names appear with an
   individual **Send** button. Verify the list scrolls without closing the
   panel.
3. Press Escape, reopen the panel, then click outside. Verify both actions
   close it and do not transmit IR.
4. Select **Download lircd.conf** and compare the saved bytes with the file
   most recently uploaded through `PUT /api/remotes`.
5. Upload a valid disposable `lircd.conf`. Verify the counts, remote selector
   and key rows refresh without reloading the page. Upload malformed text and
   verify the existing list remains active with a line-specific error.
6. With a disposable IR receiver listening, manually select a known remote and
   key. Verify the receiver reports it, the panel remains open, its status names
   the key, and the terminal receives one magenta IR annotation. Do not
   automate this transmission.

## Browser Help and OpenAPI

1. Select **Help** and verify an anchored panel opens below the button with
   UART wiring, console, file-download, IR, Wi-Fi recovery, OTA and security
   instructions. Verify opening it closes the UART or IR panel.
2. Press Escape, reopen **Help**, then click outside. Verify both actions close
   it without changing device state.
3. Follow **Open API documentation** and verify `/docs` loads without any CDN
   request, shows the running firmware version and groups the REST operations
   by Documentation, Status, UART, Files, Wi-Fi, Remotes, IR, OTA and MCP.
4. Download `/openapi.json`, validate it as JSON, and verify its OpenAPI version
   is `3.1.0`, its `info.version` matches this release, and all documented
   paths on the device are represented.
5. Verify the documentation page has no “try it” action and explicitly states
   that the API is unauthenticated and restricted to trusted networks.

## MCP

1. Add `http://<displayed-ip>/mcp` to Codex or OpenCode and verify that it
   connects without authentication or a local proxy.
2. List tools and verify that all 13 tools documented in `docs/spec.md` appear.
3. Call `status` and verify firmware version, Wi-Fi address, UART 115200 8N1,
   4 MiB history capacity, and ready IR status.
4. Call `read` without `since`, then call it again with the returned `next`.
   Verify monotonic offsets and that Base64 decodes to the exact UART bytes.
5. Call `wait_for` with a fresh offset and a known output pattern. Verify the
   match, capture and returned `next` offset.
6. At a shell prompt, call `send_and_wait` with a harmless command and the
   configured prompt expression. Verify the complete response and that no
   concurrent browser keystroke is interleaved. Verify the browser receives
   one bold magenta `[MCP]` annotation containing the command.
7. Change UART configuration to the same 115200 8N1 values and verify both MCP
   `status` and `/health`. Do not select a different baud while useful UART
   traffic is active unless that change is the test objective.
8. Load `tests/fixtures/space-enc/lircd.conf` with `remotes_load`, then verify
   `remotes_list` contains `TEST_SPACE_ENC` and `KEY_POWER`.
9. With a disposable IR receiver listening, manually send `TEST_SPACE_ENC`
   key `KEY_POWER` with two repeats. Verify the receiver reports the expected
   key activity. In the browser terminal, verify one bold magenta
   `[stb-buddy IR]` annotation naming the remote, key and repeat count. Do not
   run this transmission automatically.
10. Send harmless text through `POST /api/uart/write` and verify the browser
    receives a bold magenta `[HTTP]` annotation. Type harmless text directly
    in the browser and verify it is not duplicated as an annotation.
11. Put known disposable text into the UART history, call `clear_history`, and
    verify that neither MCP `read` nor the browser can retrieve it afterward;
    also verify that prior UART-write annotations cannot be fetched again.
12. Confirm that IR and UART-write annotations do not appear in `/api/log`,
    MCP `read`, or a `wait_for` capture, and that annotation events do not
    change UART offsets.
13. Start an MCP `wait_for` for a pattern that will not arrive, with a five
    second timeout. While it waits, repeatedly fetch `/health` and verify the
    responses are prompt. Repeat with two simultaneous waits, then verify a
    third simultaneous MCP request returns HTTP 503 with `Retry-After: 1`.

## LIRC validation limits

1. Run `tests/run_lircd_host_tests.sh` and verify both bundled remotes and all
   boundary checks pass.
2. Upload a configuration with `frequency 19999`, then one with `frequency
   60001`. Verify each is rejected and the previous remote list remains active.
3. Repeat with `duty_cycle 0`, `duty_cycle 100`, `gap 0`, and `gap 1000001`.
   Verify each is rejected without replacing the active configuration.
4. Upload configurations using the accepted endpoints (`frequency 20000` and
   `60000`, `duty_cycle 1` and `99`, `gap 1` and `1000000`) without sending a
   key. Verify each parses successfully.

## STB file download

1. Leave the connected STB at a shell prompt and create or select a harmless
   file with known bytes. Record `wc -c` and `sha256sum` for that file on the
   STB.
2. Select **Download file…** in the browser, enter its path, and start the
   download. Verify that the dialog reports shell check, preflight and transfer
   progress while the rest of the page remains responsive.
3. Verify the saved browser file is byte-identical and its size and SHA-256
   match the values recorded on the STB. Verify the completed status reports
   `serial` for a file of at most 64 KiB.
4. Call the MCP `download` tool for the same path. Verify its size, source and
   local hashes, transport and `download_url`, then fetch that URL and compare
   the bytes again.
5. Repeat with a file just over 64 KiB. If STB `nc` can connect to AtomS3R,
   verify transport `nc`; otherwise verify clean fallback to verified serial
   chunks. Confirm `ifconfig` addresses are reported when the network path was
   considered.
6. Temporarily leave the console somewhere other than a shell prompt and
   verify the READY check fails without storing a partial file. Restore the
   prompt manually before continuing.
7. Try a path that does not exist and a file larger than 2 MiB. Verify each job
   fails with a useful bounded error and exposes no stale/partial file URL.
8. Start a slow download and try another one concurrently. Verify the second
   request reports busy and does not disturb the first transfer.

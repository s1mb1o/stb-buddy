# Build status

Last verified: 2026-10-08

## Environment

- Target: `esp32s3`
- ESP-IDF: v5.5.5
- Project version: 0.12.3
- Flash layout: 8 MiB with two 3 MiB OTA application slots
- Bootloader application rollback: enabled
- ESP-IDF reproducible-build mode: enabled

## Reproducible build

Run from the repository root:

```sh
docker run --rm \
  -v "$PWD:/project" \
  -w /project \
  espressif/idf:v5.5.5 \
  idf.py set-target esp32s3 build
```

For a local ESP-IDF installation:

```sh
. /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py size
```

The application image is `build/stb_buddy.bin`. Flashing and OTA instructions
are in [README.md](README.md).

## Host tests

The default test suite is self-contained:

```sh
tests/run_lircd_host_tests.sh
```

It parses the bundled SPACE_ENC and RC5 fixtures, creates every named waveform,
and checks the accepted frequency, duty-cycle and gap boundaries. An additional
fixture directory can be supplied as the first argument.

## Verification result

The 0.12.3 release was built from a clean source snapshot with ESP-IDF v5.5.5.
Reproducible-build mode removes build timestamps and local source paths.
The exact image size, free OTA space and SHA-256 are recorded below.

- Application image: 1,763,440 bytes (`0x1ae870`)
- Free space in each 3 MiB OTA slot: 1,382,288 bytes (`0x151790`, 44 percent)
- Application SHA-256:
  `d48e608ae2bc5d5a61a503745d16d5d57848debf07bf0916e3f3d8c33be88c1a`
- ESP-IDF size report before binary padding: 1,763,317 bytes
- Reproducibility check: two clean builds in different source directories
  produced byte-identical application images

The bundled host tests and an additional private compatibility fixture suite
passed on 2026-10-08. Private fixture names and paths are intentionally not
part of this public repository.

## Hardware verification

Earlier firmware revisions were exercised on an AtomS3R with 8 MiB flash and
8 MiB PSRAM. The checks covered:

- setup AP, DHCP, port-80 provisioning and station operation;
- display pages, QR codes and button navigation;
- browser UART console, retained history and runtime UART settings;
- REST and Streamable HTTP MCP clients;
- LIRC upload, named-key IR transmission and receiver-visible key events;
- verified serial/network file download;
- OTA image validation, upload, restart and rollback state;
- browser IR, UART-write and MCP annotations.

Version 0.12.3 changes MCP request scheduling and LIRC bounds. Before publishing
a device release, repeat the applicable steps in [SMOKE_TESTS.md](SMOKE_TESTS.md),
including HTTP responsiveness during a long MCP call.

# stb-buddy: agent rules

`stb-buddy` is a standalone M5Stack AtomS3R appliance. It owns one STB UART,
serves that UART to MCP clients over Wi-Fi, and drives the AtomS3R IR LED. It
must not require a Linux companion process at runtime.

## Source of truth

- Product behavior is defined in [docs/spec.md](docs/spec.md).
- Update the specification before implementing a new externally visible tool
  or behavior.
- Record user-visible changes in [CHANGELOG.md](CHANGELOG.md).

## Implementation rules

- Target ESP32-S3 with ESP-IDF v5.5.5 and C++.
- Prefer bounded allocation. Runtime data structures must have documented
  limits and must not grow with UART traffic or client count.
- Keep UART RX independent from HTTP/MCP clients. A slow or disconnected
  client must never block the UART reader.
- Preserve UART bytes exactly. Text decoding belongs at the API boundary.
- Store Wi-Fi credentials and future API credentials in NVS, never in the
  repository.
- Do not write continuous UART history to internal flash; use PSRAM to avoid
  flash wear.
- Do not send bytes to a real STB or transmit IR during automated tests.
- Flashing hardware and destructive bootloader/flash commands require an
  explicit user request.

## Build

The reproducible build uses the official container:

```sh
docker run --rm -v "$PWD:/project" -w /project \
  espressif/idf:v5.5.5 idf.py set-target esp32s3 build
```


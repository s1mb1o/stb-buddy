# Security policy

## Supported versions

Security fixes are made on the latest release. Update to the newest published
firmware before reporting a problem that may already be fixed.

## Reporting a vulnerability

Use the repository's private GitHub security-advisory form. Do not include Wi-Fi
credentials, UART logs, firmware dumps, device addresses, or other sensitive
device data in a public issue.

## Security model

stb-buddy is a trusted-LAN development tool, not an Internet-facing service.
The HTTP, REST, and MCP interfaces have no authentication or TLS. The Wi-Fi
setup access point is open. Any client that can reach the device can control the
attached UART and IR transmitter, replace configuration, erase retained data,
or install firmware.

Wi-Fi credentials and the remote configuration are stored in NVS. Flash and NVS
encryption are not enabled, so a party with physical flash access can recover
them. OTA images are structurally validated and restricted to the `stb_buddy`
application name, but secure boot and cryptographic author verification are not
enabled.

Use an isolated, trusted network; keep physical control of the device; and
install firmware only from a trusted release.

#!/usr/bin/env python3
"""Create original SVG and PNG drawings from the firmware display layout.

Run with Python 3, qrcode 8.2, and rsvg-convert on PATH.
Network values are examples. Do not use the illustrated Wi-Fi QR for setup.
"""

from html import escape
from pathlib import Path
import shutil
import subprocess

import qrcode


# Original 5 x 7 pixel glyphs. Each display cell has a 6 x 8 pixel advance.
GLYPHS = {
    " ": "00000/00000/00000/00000/00000/00000/00000",
    "A": "01110/10001/10001/11111/10001/10001/10001",
    "B": "11110/10001/10001/11110/10001/10001/11110",
    "C": "01111/10000/10000/10000/10000/10000/01111",
    "D": "11110/10001/10001/10001/10001/10001/11110",
    "E": "11111/10000/10000/11110/10000/10000/11111",
    "F": "11111/10000/10000/11110/10000/10000/10000",
    "G": "01111/10000/10000/10111/10001/10001/01111",
    "H": "10001/10001/10001/11111/10001/10001/10001",
    "I": "11111/00100/00100/00100/00100/00100/11111",
    "J": "00111/00010/00010/00010/10010/10010/01100",
    "K": "10001/10010/10100/11000/10100/10010/10001",
    "L": "10000/10000/10000/10000/10000/10000/11111",
    "M": "10001/11011/10101/10101/10001/10001/10001",
    "N": "10001/11001/10101/10011/10001/10001/10001",
    "O": "01110/10001/10001/10001/10001/10001/01110",
    "P": "11110/10001/10001/11110/10000/10000/10000",
    "Q": "01110/10001/10001/10001/10101/10010/01101",
    "R": "11110/10001/10001/11110/10100/10010/10001",
    "S": "01111/10000/10000/01110/00001/00001/11110",
    "T": "11111/00100/00100/00100/00100/00100/00100",
    "U": "10001/10001/10001/10001/10001/10001/01110",
    "V": "10001/10001/10001/10001/10001/01010/00100",
    "W": "10001/10001/10001/10101/10101/10101/01010",
    "X": "10001/10001/01010/00100/01010/10001/10001",
    "Y": "10001/10001/01010/00100/00100/00100/00100",
    "Z": "11111/00001/00010/00100/01000/10000/11111",
    "0": "01110/10001/10011/10101/11001/10001/01110",
    "1": "00100/01100/00100/00100/00100/00100/01110",
    "2": "01110/10001/00001/00010/00100/01000/11111",
    "3": "11110/00001/00001/01110/00001/00001/11110",
    "4": "00010/00110/01010/10010/11111/00010/00010",
    "5": "11111/10000/10000/11110/00001/00001/11110",
    "6": "01110/10000/10000/11110/10001/10001/01110",
    "7": "11111/00001/00010/00100/01000/01000/01000",
    "8": "01110/10001/10001/01110/10001/10001/01110",
    "9": "01110/10001/10001/01111/00001/00001/01110",
    "a": "00000/00000/01110/00001/01111/10001/01111",
    "b": "10000/10000/10110/11001/10001/10001/11110",
    "d": "00001/00001/01101/10011/10001/10001/01111",
    "i": "00100/00000/01100/00100/00100/00100/01110",
    "u": "00000/00000/10001/10001/10001/10011/01101",
    "y": "00000/00000/10001/10001/01111/00001/01110",
    "-": "00000/00000/00000/11111/00000/00000/00000",
    ".": "00000/00000/00000/00000/00000/00110/00110",
    ":": "00000/00110/00110/00000/00110/00110/00000",
    "<": "00010/00100/01000/10000/01000/00100/00010",
    ">": "01000/00100/00010/00001/00010/00100/01000",
}


def rgb565(value):
    r = ((value >> 11) & 31) * 255 // 31
    g = ((value >> 5) & 63) * 255 // 63
    b = (value & 31) * 255 // 31
    return f"#{r:02x}{g:02x}{b:02x}"


BG, PANEL, WHITE, MUTED, GREEN, YELLOW, BLUE = map(
    rgb565, (0x0861, 0x10E3, 0xFFFF, 0x9CF3, 0x4E69, 0xFDC0, 0x2D7F)
)
AP_SSID = "Buddy-AABBCCDDEEFF"
STATION_SSID = "Lab-WiFi"
STATION_IP = "192.168.1.50"
WIFI_PAYLOAD = f"WIFI:T:nopass;S:{AP_SSID};;"
URL_PAYLOAD = "http://192.168.4.1/"


def rect(x, y, w, h, color, radius=0):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" fill="{color}"/>'


def text(value, x, y, color=WHITE, anchor="top_center"):
    if anchor in ("top_center", "middle_center"):
        x -= len(value) * 6 / 2
    if anchor == "middle_center":
        y -= 4
    cells = []
    for index, char in enumerate(value):
        for row, bits in enumerate(GLYPHS[char].split("/")):
            for col, bit in enumerate(bits):
                if bit == "1":
                    cells.append(f"M{x + index * 6 + col:g},{y + row:g}h1v1h-1z")
    return f'<path fill="{color}" shape-rendering="crispEdges" d="{"".join(cells)}"><title>{escape(value)}</title></path>'


def main_screen(state, ssid, ip, hold=False):
    color = {"ONLINE": GREEN, "WIFI SETUP": BLUE, "CONNECTING": YELLOW}[state]
    elements = [
        rect(0, 0, 128, 128, BG),
        rect(4, 4, 120, 24, PANEL, 6),
        text("STB BUDDY", 64, 16, anchor="middle_center"),
        text(state, 64, 36, color, "middle_center"),
        text("SSID", 8, 46, MUTED, "top_left"),
        text(ssid, 64, 58),
        text("IP ADDRESS", 8, 78, MUTED, "top_left"),
        text(ip, 64, 90),
        text("PRESS: QR  WEB :80" if state == "WIFI SETUP" else "PRESS:PINS HOLD:SETUP", 64, 109, MUTED),
    ]
    if hold:
        elements.extend([
            '<rect x="8" y="121" width="112" height="4" rx="2" stroke="' + MUTED + '" stroke-width="1" fill="none"/>',
            rect(10, 122, 81, 2, BLUE, 1),
        ])
    return "\n".join(elements)


def wiring_screen():
    return "\n".join([
        rect(0, 0, 128, 128, BG), rect(4, 4, 120, 22, PANEL, 6),
        text("UART CONNECTION", 64, 15, anchor="middle_center"),
        text("ATOM S3R       STB", 64, 30, MUTED),
        text("GND    <-->    GND", 64, 44),
        text("G2 TXD  -->   RXD", 64, 58),
        text("G1 RXD  <--   TXD", 64, 72),
        text("3.3V UART ONLY", 64, 90, YELLOW),
        text("USB: LEAVE 5V OPEN", 64, 103, YELLOW),
        text("PRESS: BACK", 64, 117, MUTED),
    ])


def qr_screen(title, payload):
    # M5GFX starts at QR version 3 and uses ECC_LOW (0).
    code = qrcode.QRCode(version=3, error_correction=qrcode.constants.ERROR_CORRECT_L,
                         box_size=1, border=0)
    code.add_data(payload, optimize=0)
    code.make(fit=False)
    matrix = code.get_matrix()
    size = len(matrix)
    thickness = 112 // size
    offset = (112 - size * thickness) // 2
    if offset < thickness * 4:
        thickness = (112 - thickness * 8) // size
        offset = (112 - size * thickness) // 2
    elements = [rect(0, 0, 128, 128, "#000000"), text(title, 64, 1),
                rect(8, 10, 112, 112, WHITE)]
    for y, row in enumerate(matrix):
        for x, dark in enumerate(row):
            if dark:
                elements.append(rect(8 + offset + x * thickness,
                                     10 + offset + y * thickness,
                                     thickness, thickness, "#000000"))
    return "\n".join(elements)


def device(title, screen):
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="1200" viewBox="0 0 600 600" role="img" aria-labelledby="title description">
<title id="title">{escape(title)}</title>
<desc id="description">Front-view drawing of an M5Stack AtomS3R. Only the device and its screen are shown. Network values are examples.</desc>
<defs>
  <linearGradient id="case" x1="0" y1="0" x2="0.8" y2="1">
    <stop offset="0" stop-color="#f5f5f2"/><stop offset="0.45" stop-color="#e2e3df"/><stop offset="1" stop-color="#caccC8"/>
  </linearGradient>
  <linearGradient id="well" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#a5a9a5"/><stop offset="1" stop-color="#f6f6f3"/>
  </linearGradient>
  <clipPath id="lcd"><rect width="128" height="128" rx="0.7"/></clipPath>
</defs>
<rect x="40" y="40" width="520" height="520" rx="62" fill="url(#case)" stroke="#bfc2bd" stroke-width="2"/>
<rect x="45" y="45" width="510" height="510" rx="58" fill="none" stroke="#ffffff" stroke-opacity="0.65" stroke-width="2"/>
<rect x="106" y="106" width="388" height="388" rx="22" fill="url(#well)"/>
<rect x="118" y="118" width="364" height="364" rx="10" fill="#262c2e" stroke="#737875" stroke-width="2"/>
<g transform="translate(124 124) scale(2.75)" clip-path="url(#lcd)">
{screen}
</g>
</svg>
'''


def main():
    renderer = shutil.which("rsvg-convert")
    if renderer is None:
        raise SystemExit("Install librsvg to provide rsvg-convert.")
    output = Path(__file__).resolve().parents[1] / "images"
    output.mkdir(parents=True, exist_ok=True)
    screens = {
        "main-online": ("Main screen: ONLINE", main_screen("ONLINE", STATION_SSID, STATION_IP)),
        "main-setup": ("Main screen: WIFI SETUP", main_screen("WIFI SETUP", AP_SSID, "192.168.4.1")),
        "main-connecting": ("Main screen: CONNECTING", main_screen("CONNECTING", STATION_SSID, "--")),
        "setup-hold": ("Main screen: setup button hold at 1.5 seconds", main_screen("ONLINE", STATION_SSID, STATION_IP, hold=True)),
        "uart-wiring": ("UART connection screen", wiring_screen()),
        "ap-wifi-qr": ("AP Wi-Fi QR screen: example network", qr_screen("JOIN SETUP WIFI", WIFI_PAYLOAD)),
        "ap-setup-url": ("AP setup URL QR screen", qr_screen("OPEN SETUP PAGE", URL_PAYLOAD)),
    }
    for name, (title, screen) in screens.items():
        svg = output / f"{name}.svg"
        png = output / f"{name}.png"
        svg.write_text(device(title, screen), encoding="utf-8")
        subprocess.run([renderer, "--output", str(png), str(svg)], check=True)
        print(f"Created {svg.name} and {png.name}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Independently generated sensor fixture. No captured/vendor/learned input."""
import base64
import math
from pathlib import Path
import struct

width, height, count = 33, 25, 4
black, white = 64, 4095
payload = bytearray(b'LBR1' + struct.pack('<6I', width, height, count, 0, black, white))
state = 2187
for frame in range(count):
    # Six independent discrete uniforms -4..4: variance = 40 code units squared.
    payload += struct.pack('<qqff', 1_000_000_000 + frame * 30_000_000, 20_000_000, 100.0, 40.0 / (white-black)**2)
    for y in range(height):
        for x in range(width):
            base = 0.10 + 0.11*x/(width-1) + 0.055*math.sin(x*.35)*math.cos(y*.27)
            base += 0.12 if 9 < x < 23 and 5 < y < 19 else 0
            channel = (0 if x%2 == 0 else 1) if y%2 == 0 else (1 if x%2 == 0 else 2)
            color = (1.0, 0.85, 0.65)[channel]
            noise = 0
            for _ in range(6):
                state = (1664525*state + 1013904223) & 0xffffffff
                noise += (state >> 16) % 9 - 4
            value = round(black + (white-black)*base*color) + noise
            payload += struct.pack('<H', max(0, min(65535, value)))
encoded = base64.b64encode(payload).decode('ascii')
path = Path(__file__).resolve().parents[2] / 'android/app/src/main/assets/static_burst.b64'
path.parent.mkdir(parents=True, exist_ok=True)
path.write_text('\n'.join(encoded[i:i+96] for i in range(0,len(encoded),96))+'\n', encoding='ascii')
print(f'{path}: {len(payload)} bytes; {count} frames; synthetic linear-sRGB sensor')

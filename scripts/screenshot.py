#!/usr/bin/env python3
"""Capture the virtio GPU output using QMP; the device ID must be 'vgpu'."""
import argparse
import json
from pathlib import Path
import socket

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--port', type=int, default=4445)
p.add_argument('--boot-framebuffer', action='store_true', help='Capture the default console (ramfb in the provided launcher); this is not GL scanout evidence')
a = p.parse_args()
a.output.resolve().parent.mkdir(parents=True, exist_ok=True)
with socket.create_connection(('127.0.0.1', a.port), timeout=10) as s:
    f = s.makefile('rwb', buffering=0)
    json.loads(f.readline())
    capture = {'filename': str(a.output.resolve()), 'format': 'png'}
    if not a.boot_framebuffer:
        capture.update(device='vgpu', head=0)
    for name, args in [('qmp_capabilities', {}), ('screendump', capture)]:
        f.write((json.dumps({'execute': name, 'arguments': args}) + '\n').encode())
        while True:
            raw = f.readline()
            if not raw:
                raise RuntimeError('QMP disconnected')
            reply = json.loads(raw)
            if 'error' in reply:
                raise RuntimeError(reply['error'])
            if 'return' in reply:
                break
if a.boot_framebuffer:
    print('Source: default boot/ramfb console, not the GL scanout texture')
print(a.output.resolve())

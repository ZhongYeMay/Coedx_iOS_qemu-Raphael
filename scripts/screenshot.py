#!/usr/bin/env python3
"""Capture the virtio GPU output using QMP; the device ID must be 'vgpu'."""
import argparse
import json
from pathlib import Path
import socket

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--port', type=int, default=4445)
a = p.parse_args()
a.output.resolve().parent.mkdir(parents=True, exist_ok=True)
with socket.create_connection(('127.0.0.1', a.port), timeout=10) as s:
    f = s.makefile('rwb', buffering=0)
    json.loads(f.readline())
    for name, args in [('qmp_capabilities', {}), ('screendump', {
        'filename': str(a.output.resolve()), 'device': 'vgpu', 'head': 0, 'format': 'png'
    })]:
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
print(a.output.resolve())

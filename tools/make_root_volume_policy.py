#!/usr/bin/env python3
"""Generate a private exact-volume guard from a qualified HFSX image.

No mounts or phone API. Never changes the input image/template.
"""
from pathlib import Path
import argparse
import hashlib
import re
import struct

TOTAL = 16 << 30


def check_header(h):
    if len(h) != 512 or h[:4] != b'HX\x00\x05':
        raise ValueError('Expected complete HFSX v5 header')
    if struct.unpack_from('>II', h, 40) != (4096, 4194304):
        raise ValueError('Expected 4096-byte blocks and exact16GiB volume')
    if h[104:112] == bytes(8):
        raise ValueError('Zero volume identity')
    return h[104:112]


def render(template, first, last):
    identity = check_header(first)
    if check_header(last) != identity:
        raise ValueError('Primary/alternate volume identity differs')
    pattern = r'static const uint8_t id\[8\]=\{[^}]*\};'
    replacement = 'static const uint8_t id[8]={' + ','.join('0x%02x' % x for x in identity) + '};'
    if len(re.findall(pattern, template)) != 1:
        raise ValueError('Expected one explicit volume policy; refusing a broad replacement')
    return re.sub(pattern, lambda _: replacement, template)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--expected-sha256', required=True)
    p.add_argument('--template', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if a.image.is_symlink() or not a.image.is_file() or a.image.stat().st_size != TOTAL:
        raise ValueError('Expected qualified regular16GiB source')
    if not re.fullmatch('[0-9a-f]{64}', a.expected_sha256):
        raise ValueError('Expected explicit SHA256')
    h = hashlib.sha256()
    with a.image.open('rb') as f:
        for data in iter(lambda: f.read(8 << 20), b''):
            h.update(data)
        if h.hexdigest() != a.expected_sha256:
            raise ValueError('Source SHA mismatch')
        f.seek(1024)
        first = f.read(512)
        f.seek(TOTAL - 1024)
        last = f.read(512)
    data = render(a.template.read_text(), first, last)
    with a.output.open('x') as f:
        f.write(data)
    a.output.chmod(0o600)


if __name__ == '__main__':
    main()

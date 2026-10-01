#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarise a guest-code profile (logs/profile.bin, tools/console-run.py --profile-at).

  tools/analyze-profile.py results/runs/LABEL-profile.bin [--top 40] [--disassemble N]

Attributes each host PC sample of guest cores 0-2 to the JIT block it fell in and so to a
guest address, and ranks guest code by samples: per block, and per 4 KiB of guest code.
With --disassemble, prints the AArch64 code of the N hottest blocks (llvm-objdump).
"""
import argparse
import bisect
import collections
import struct
import subprocess
import tempfile

PC_MASK = (1 << 39) - 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('profile')
    parser.add_argument('--top', type=int, default=40)
    parser.add_argument('--disassemble', type=int, default=0)
    args = parser.parse_args()
    data = open(args.profile, 'rb').read()
    at = 0
    total = jit = 0
    per_block = collections.Counter()
    per_page = collections.Counter()
    outside = collections.Counter()
    for core in range(3):
        blocks, samples = struct.unpack_from('<QQ', data, at)
        at += 16
        table = [struct.unpack_from('<QQQ', data, at + i * 24) for i in range(blocks)]
        at += blocks * 24
        entries = [b[0] for b in table]
        pcs = struct.unpack_from(f'<{samples}Q', data, at)
        at += samples * 8
        for pc in pcs:
            total += 1
            i = bisect.bisect_right(entries, pc) - 1
            if i >= 0 and pc - table[i][0] < table[i][1]:
                jit += 1
                location = table[i][2] & PC_MASK
                per_block[location] += 1
                per_page[location >> 12] += 1
            else:
                outside[pc >> 20] += 1
    code_blocks = struct.unpack_from('<Q', data, at)[0]
    at += 8
    code = {}
    for _ in range(code_blocks):
        location = struct.unpack_from('<Q', data, at)[0] & PC_MASK
        code[location] = data[at + 8:at + 8 + 256]
        at += 8 + 256
    print(f'samples {total}: in JIT blocks {jit / total * 100:.1f}%, elsewhere {100 - jit / total * 100:.1f}%')
    print('elsewhere by 1 MiB of host address:', [(hex(k << 20), v) for k, v in outside.most_common(6)])
    cumulative = 0
    print(f'\nhottest guest blocks ({len(per_block)} blocks sampled):')
    for location, count in per_block.most_common(args.top):
        cumulative += count
        print(f'  {location:#012x} {count / jit * 100:5.2f}%  cumulative {cumulative / jit * 100:5.1f}%')
    print('\nhottest 4 KiB of guest code:')
    cumulative = 0
    for page, count in per_page.most_common(25):
        cumulative += count
        print(f'  {page << 12:#012x} {count / jit * 100:5.2f}%  cumulative {cumulative / jit * 100:5.1f}%')
    for location, count in per_block.most_common(args.disassemble):
        if location not in code:
            continue
        with tempfile.NamedTemporaryFile(suffix='.bin') as f:
            f.write(code[location])
            f.flush()
            text = subprocess.run(['llvm-objdump', '-D', '-b', 'binary', '--triple=aarch64', f'--adjust-vma={location}', f.name],
                                  capture_output=True, text=True).stdout
        lines = [l for l in text.splitlines() if l.strip()[:1].isalnum() and ':' in l][:40]
        print(f'\n=== {location:#x} ({count / jit * 100:.2f}%)')
        print('\n'.join(lines))


if __name__ == '__main__':
    main()

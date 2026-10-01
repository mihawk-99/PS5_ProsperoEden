#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarise a development thread timeline (logs/sched.bin, tools/console-run.py --trace-at).

  tools/analyze-trace.py results/runs/LABEL-sched.bin [--threads 16]

Per guest thread: time running on the emulated cores per frame, its supervisor calls and the
time it spent inside them (blocked or served), and the cores it ran on. Per core: idle time.
Frames are the game's queued buffers.
"""
import argparse
import collections
import struct

TYPES = ['RunBegin', 'RunEnd', 'SvcBegin', 'SvcEnd', 'IdleBegin', 'IdleEnd', 'Frame']
SVC_NAMES = {
    0x01: 'SetHeapSize', 0x06: 'QueryMemory', 0x07: 'ExitProcess', 0x08: 'CreateThread', 0x09: 'StartThread',
    0x0A: 'ExitThread', 0x0B: 'SleepThread', 0x0C: 'GetThreadPriority', 0x0D: 'SetThreadPriority',
    0x0E: 'GetThreadCoreMask', 0x0F: 'SetThreadCoreMask', 0x10: 'GetCurrentProcessorNumber',
    0x11: 'SignalEvent', 0x12: 'ClearEvent', 0x13: 'MapSharedMemory', 0x16: 'CloseHandle', 0x17: 'ResetSignal',
    0x18: 'WaitSynchronization', 0x19: 'CancelSynchronization', 0x1A: 'ArbitrateLock', 0x1B: 'ArbitrateUnlock',
    0x1C: 'WaitProcessWideKeyAtomic', 0x1D: 'SignalProcessWideKey', 0x1E: 'GetSystemTick', 0x1F: 'ConnectToNamedPort',
    0x21: 'SendSyncRequest', 0x22: 'SendSyncRequestWithUserBuffer', 0x25: 'GetThreadId', 0x26: 'Break',
    0x27: 'OutputDebugString', 0x29: 'GetInfo', 0x2C: 'MapPhysicalMemory', 0x34: 'WaitForAddress',
    0x35: 'SignalToAddress', 0x36: 'SynchronizePreemptionState', 0x40: 'CreateSession', 0x43: 'ReplyAndReceive',
    0x45: 'CreateEvent', 0x77: 'MapProcessCodeMemory',
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('trace')
    parser.add_argument('--threads', type=int, default=16)
    args = parser.parse_args()
    data = open(args.trace, 'rb').read()
    events = [struct.unpack_from('<qIHBBQQ', data, i) for i in range(0, len(data) - 31, 32)]
    events.sort(key=lambda e: e[0])
    start, end = events[0][0], events[-1][0]
    seconds = (end - start) / 1e9

    frames = [e[0] for e in events if TYPES[e[3]] == 'Frame']
    frame_count = max(len(frames) - 1, 1)
    if len(frames) > 1:
        gaps = [(b - a) / 1e6 for a, b in zip(frames, frames[1:])]
        print(f'{seconds:.2f} s, {len(events)} events, {len(frames)} frames: '
              f'{len(frames) / seconds:.1f} fps, frame ms mean {sum(gaps) / len(gaps):.1f} '
              f'min {min(gaps):.1f} max {max(gaps):.1f}')

    run = collections.Counter()
    run_cores = collections.defaultdict(collections.Counter)
    priority = {}
    running = {}
    svc_time = collections.defaultdict(collections.Counter)
    svc_count = collections.defaultdict(collections.Counter)
    in_svc = {}
    idle = collections.Counter()
    idle_since = {}
    sleeps = collections.Counter()
    for ns, thread, core, kind, svc, a0, a1 in events:
        kind = TYPES[kind]
        if kind == 'RunBegin':
            running[core] = (thread, ns)
            priority[thread] = a0
        elif kind == 'RunEnd':
            if core in running and running[core][0] == thread:
                run[thread] += ns - running[core][1]
                run_cores[thread][core] += ns - running[core][1]
            running.pop(core, None)
        elif kind == 'SvcBegin':
            in_svc[thread] = (svc, ns)
        elif kind == 'SvcEnd':
            if thread in in_svc and in_svc[thread][0] == svc:
                svc_time[thread][svc] += ns - in_svc[thread][1]
                svc_count[thread][svc] += 1
            in_svc.pop(thread, None)
        elif kind == 'IdleBegin':
            idle_since[core] = ns
        elif kind == 'IdleEnd':
            if core in idle_since:
                idle[core] += ns - idle_since.pop(core)
                sleeps[core] += 1 if svc else 0

    print('\ncore  idle%   sleeps/s')
    for core in sorted(idle):
        print(f'{core:4d}  {idle[core] / (end - start) * 100:5.1f}  {sleeps[core] / seconds:8.0f}')

    print(f'\nthreads by run time (ms per frame over {frame_count} frames):')
    print('  tid   prio   run  cores(ms/frame)          top calls: name count/frame ms/frame')
    for thread, ns in run.most_common(args.threads):
        cores = ' '.join(f'c{c}:{t / 1e6 / frame_count:.1f}' for c, t in sorted(run_cores[thread].items()))
        calls = sorted(svc_time[thread].items(), key=lambda kv: -kv[1])[:4]
        call_text = ', '.join(f'{SVC_NAMES.get(s, hex(s))} {svc_count[thread][s] / frame_count:.0f}/{t / 1e6 / frame_count:.1f}'
                              for s, t in calls)
        print(f'{thread:6d} {priority.get(thread, 0):5d} {ns / 1e6 / frame_count:5.1f}  {cores:24s} {call_text}')


if __name__ == '__main__':
    main()

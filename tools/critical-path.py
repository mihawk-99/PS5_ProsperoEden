#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Critical path of each frame in a development thread timeline (logs/sched.bin).

  tools/critical-path.py results/runs/LABEL-sched.bin [--frames 40] [--verbose]

Starts at the guest thread that is in a service request when a frame is queued (the request
that presents it) and walks backwards one frame period. Time the walked thread was running
counts as guest code. When it was blocked in a wait that another guest thread ended (address
arbiter, lock, condition variable), the walk jumps to that thread at the moment it signalled;
the gap between the signal and the waiter running again counts as wake latency. Waits no
guest thread ended (service requests, synchronization objects, timed sleeps) count as their
own categories: those are emulator-side or timer waits.
"""
import argparse
import bisect
import collections
import struct

RUN_BEGIN, RUN_END, SVC_BEGIN, SVC_END, IDLE_BEGIN, IDLE_END, FRAME = range(7)
SEND_SYNC = {0x21, 0x22}
WAIT_ADDRESS, SIGNAL_ADDRESS = 0x34, 0x35
LOCK, UNLOCK = 0x1A, 0x1B
WAIT_KEY, SIGNAL_KEY = 0x1C, 0x1D
NAMES = {0x0B: 'SleepThread', 0x18: 'WaitSynchronization', 0x21: 'SendSyncRequest',
         0x22: 'SendSyncRequestWithUserBuffer', 0x34: 'WaitForAddress', 0x1A: 'ArbitrateLock',
         0x1C: 'WaitProcessWideKeyAtomic', 0x43: 'ReplyAndReceive'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('trace')
    parser.add_argument('--frames', type=int, default=40)
    parser.add_argument('--verbose', action='store_true')
    args = parser.parse_args()
    data = open(args.trace, 'rb').read()
    events = sorted((struct.unpack_from('<qIHBBQQ', data, i) for i in range(0, len(data) - 31, 32)),
                    key=lambda e: e[0])

    runs = collections.defaultdict(list)       # thread -> [(begin, end)]
    calls = collections.defaultdict(list)      # thread -> [(begin, end, svc, a0, a1)]
    signals = collections.defaultdict(list)    # (kind, key) -> [(ns, thread)]
    frames = []
    open_run, open_call = {}, {}
    for ns, thread, core, kind, svc, a0, a1 in events:
        if kind == RUN_BEGIN:
            open_run[core] = (thread, ns)
        elif kind == RUN_END and core in open_run and open_run[core][0] == thread:
            runs[thread].append((open_run.pop(core)[1], ns))
        elif kind == SVC_BEGIN:
            open_call[thread] = (ns, svc, a0, a1)
            if svc == SIGNAL_ADDRESS:
                signals[('address', a0)].append((ns, thread))
            elif svc == UNLOCK:
                signals[('lock', a0)].append((ns, thread))
            elif svc == SIGNAL_KEY:
                signals[('key', a0)].append((ns, thread))
        elif kind == SVC_END and thread in open_call and open_call[thread][1] == svc:
            begin, _, b0, b1 = open_call.pop(thread)
            calls[thread].append((begin, ns, svc, b0, b1))
        elif kind == FRAME:
            frames.append(ns)
    for table in (runs, calls):
        for thread in table:
            table[thread].sort()
    for key in signals:
        signals[key].sort()
    call_begins = {t: [c[0] for c in calls[t]] for t in calls}
    run_begins = {t: [r[0] for r in runs[t]] for t in runs}

    def signaller(call):
        begin, end, svc, a0, a1 = call
        key = {WAIT_ADDRESS: ('address', a0), LOCK: ('lock', a1), WAIT_KEY: ('key', a1)}.get(svc)
        if key is None or key not in signals:
            return None
        times = signals[key]
        i = bisect.bisect_right(times, (end, 1 << 62)) - 1
        while i >= 0 and times[i][0] > begin:
            if times[i][1] != None:
                return times[i]
            i -= 1
        return None

    totals = collections.Counter()
    hops = collections.Counter()
    walked = 0
    for f_index in range(1, min(len(frames), args.frames + 1)):
        frame = frames[f_index]
        period = frame - frames[f_index - 1]
        # The presenting thread: in a service request when the frame was queued.
        presenter = None
        for thread, thread_calls in calls.items():
            i = bisect.bisect_right(call_begins[thread], frame) - 1
            if i >= 0 and thread_calls[i][2] in SEND_SYNC and thread_calls[i][0] <= frame <= thread_calls[i][1]:
                presenter, at = thread, thread_calls[i][0]
                break
        if presenter is None:
            continue
        walked += 1
        stop = at - period
        thread, t = presenter, at
        path = []
        guard = 0
        while t > stop and guard < 20000:
            guard += 1
            # The latest run and call that began before t on this thread.
            r = bisect.bisect_left(run_begins.get(thread, []), t) - 1
            run = runs[thread][r] if r >= 0 else None
            c = bisect.bisect_left(call_begins.get(thread, []), t) - 1
            call = calls[thread][c] if c >= 0 else None
            if run and run[1] >= t:
                begin = max(run[0], stop)
                totals['guest code'] += t - begin
                t = begin
                continue
            if call and call[1] >= t:
                begin, svc = call[0], call[2]
                source = signaller(call) if svc in (WAIT_ADDRESS, LOCK, WAIT_KEY) else None
                if source and source[0] > stop and source[0] < t:
                    totals['wake latency (signal to running)'] += t - source[0]
                    hops[(thread, source[1])] += 1
                    path.append((thread, NAMES.get(svc, hex(svc)), source[1]))
                    thread, t = source[1], source[0]
                    continue
                name = NAMES.get(svc, hex(svc))
                if svc in (WAIT_ADDRESS, LOCK, WAIT_KEY):
                    name += ' (signalled earlier)' if source else ' (no guest signal)'
                totals[name] += t - max(begin, stop)
                t = begin
                continue
            # Between events: runnable but not running, or the trace's start.
            previous = max(run[1] if run else 0, call[1] if call else 0)
            if previous == 0 or previous <= stop:
                totals['before trace / unknown'] += t - stop
                break
            totals['runnable, waiting for a core'] += t - previous
            t = previous
        if args.verbose:
            print(f'frame {f_index}: presenter {presenter}, path {path[:12]}')
    span = sum(totals.values()) or 1
    print(f'{walked} frames walked; critical path per frame:')
    for name, ns in totals.most_common():
        print(f'  {ns / walked / 1e6:6.2f} ms  {ns / span * 100:5.1f}%  {name}')
    print('most frequent hand-offs (waiter <- signaller):', hops.most_common(8))


if __name__ == '__main__':
    main()

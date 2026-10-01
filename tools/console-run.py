#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One measured game run of the development build (make dev) on the console.

  tools/console-run.py LABEL --rom TITLE_ID [--resolution 4k] [--seconds 100]
                       [--press circle --press-from 20 --press-every 3 --presses 20]
                       [--dev SETTING ...] [--pc-sample]

Writes the resolution into prosperoeden.json and dev-settings.txt (rom=, replay=off and any
--dev entries) into the app folder, launches PPSA99008 through PS5_Vulkan's run-title.py and
optionally presses a button at intervals (development input, compat-input.txt) to get past
menus. After --seconds the game is stopped through Eden's own shutdown (stop-game.txt) and
the title exits by itself; it is never killed at the end of a run (killing a title that
renders at 8K preceded two console power-offs). The runner's timeout is only a watchdog.

Afterwards the player's resolution is restored and the development files are removed.
Logs land in results/runs/LABEL-{klog,heap,stderr}.txt with a summary of the in-game windows
(at least --min-draws draws per frame): game frames per second, GPU busy, guest core load.
Needs PS5_Vulkan beside this checkout (its .env names the console).
"""
import argparse
import ftplib
import io
import json
import pathlib
import re
import subprocess
import sys
import threading

ROOT = pathlib.Path(__file__).resolve().parents[1]
VULKAN = ROOT.parent / 'PS5_Vulkan'
APP = '/data/homebrew/PPSA99008'
DATA = '/data/prosperoeden'
BUTTONS = {'cross': 0x4000, 'circle': 0x2000, 'square': 0x8000, 'triangle': 0x1000, 'options': 0x8}
# The run ends when main has returned after the clean stop (the title then idles, its game and
# renderer released, and closing it is safe) or on a failure.
UNTIL = r'EDEN_PPSA99121_MAIN_RETURN|session failed|fatal signal|GPU_FAULT|gpu fault'


def settings():
    values = {}
    for raw in (VULKAN / '.env').read_text().splitlines():
        line = raw.strip()
        if line and not line.startswith('#') and '=' in line:
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip().strip('"')
    return values


def connect(env):
    client = ftplib.FTP()
    client.connect(env['PS5_HOST'], int(env.get('FTP_PORT', '2121')), timeout=30)
    client.login(env.get('PS5_FTP_USER') or 'anonymous', env.get('PS5_FTP_PASSWORD') or '')
    return client


def put(client, path, text):
    client.storbinary('STOR ' + path, io.BytesIO(text.encode()))


def get(client, path):
    data = io.BytesIO()
    try:
        client.retrbinary('RETR ' + path, data.write)
    except ftplib.all_errors:
        return None
    return data.getvalue().decode(errors='replace')


def delete(client, path):
    try:
        client.delete(path)
    except ftplib.all_errors:
        pass


def summarise(heap, min_draws):
    """Per 5 s window: game frames/s, draws per frame, GPU busy, guest cores 0-2 busy."""
    rows, previous, gpu_busy, cores = [], None, 0.0, {}
    core_prev = {}
    for line in heap.splitlines():
        if line.startswith('EDEN_GPU_TIME'):
            values = dict(re.findall(r'(\w+)=([\d.]+)', line))
            gpu_busy = float(values['busy_ms']) / float(values['wall_ms'])
        elif line.startswith('EDEN_PERF_CPU_POINT'):
            values = dict(re.findall(r'(\w+)=(-?[0-9a-fx]+)', line))
            core, mono, cpu = int(values['core']), int(values['mono_ns']), int(values['cpu_ns'])
            if mono and core < 3:
                if core in core_prev and mono > core_prev[core][0]:
                    cores[core] = (cpu - core_prev[core][1]) / (mono - core_prev[core][0])
                core_prev[core] = (mono, cpu)
        elif line.startswith('EDEN_DEV_GPU'):
            values = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)}
            if previous:
                seconds = (values['mono_ns'] - previous['mono_ns']) / 1e9
                fps = (values['frame'] - previous['frame']) / seconds
                draws = (values['draws'] - previous['draws']) / seconds / max(fps, 1)
                rows.append((fps, draws, gpu_busy, [cores.get(c, 0.0) for c in range(3)]))
            previous = values
    game = [row for row in rows if row[1] >= min_draws][2:]  # skip the first in-game windows
    if not game:
        return 'no in-game windows'
    fps = [row[0] for row in game]
    load = [sum(row[3][c] for row in game) / len(game) * 100 for c in range(3)]
    return (f'in-game windows={len(game)} fps mean={sum(fps) / len(fps):.1f} min={min(fps):.1f} '
            f'max={max(fps):.1f} draws/frame={sum(r[1] for r in game) / len(game):.0f} '
            f'gpu_busy={sum(r[2] for r in game) / len(game) * 100:.0f}% '
            f'cores={load[0]:.0f}/{load[1]:.0f}/{load[2]:.0f}%')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('label')
    parser.add_argument('--rom', required=True, help='title ID of the game to boot')
    parser.add_argument('--resolution', default='4k', choices=['720p', '1080p', '4k', '8k'])
    parser.add_argument('--seconds', type=float, default=100, help='play time before the clean stop')
    parser.add_argument('--dev', action='append', default=[], help='extra dev-settings.txt entry')
    parser.add_argument('--press', choices=sorted(BUTTONS), help='button to press at intervals')
    parser.add_argument('--press-from', type=float, default=20)
    parser.add_argument('--press-every', type=float, default=3)
    parser.add_argument('--presses', type=int, default=20)
    parser.add_argument('--pc-sample', action='store_true', help='host PC sampling (pc-sample.txt)')
    parser.add_argument('--min-draws', type=float, default=800, help='draws per frame of an in-game window')
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9A-Fa-f]{16}', args.rom):
        raise SystemExit('--rom must be a 16-digit title ID')

    env = settings()
    out = ROOT / 'results' / 'runs'
    out.mkdir(parents=True, exist_ok=True)
    client = connect(env)
    config = json.loads(get(client, f'{DATA}/config/prosperoeden.json') or '{}')
    chosen_resolution = config.get('video', {}).get('resolution')  # restored after the run
    config.setdefault('video', {})['resolution'] = args.resolution
    put(client, f'{DATA}/config/prosperoeden.json', json.dumps(config, indent=2) + '\n')
    put(client, f'{APP}/dev-settings.txt', '\n'.join([f'rom={args.rom.upper()}', 'replay=off', 'gpu_time=on'] + args.dev) + '\n')
    for name in ('stop-game.txt', 'compat-input.txt', 'pc-sample.txt', 'cost-run.txt', 'capture-once.txt'):
        delete(client, f'{APP}/{name}')
    if args.pc_sample:
        put(client, f'{APP}/pc-sample.txt', '1\n')
    for name in ('heap.log', 'stderr.log'):
        delete(client, f'{DATA}/logs/{name}')
    client.quit()

    done = threading.Event()

    def presses():
        if not args.press or done.wait(args.press_from):
            return
        session = connect(env)
        for sequence in range(1, args.presses + 1):
            try:
                put(session, f'{APP}/compat-input.txt', f'{sequence} {BUTTONS[args.press]} 200\n')
            except ftplib.all_errors:
                session = connect(env)
            if done.wait(args.press_every):
                return

    def stop():
        if done.wait(args.seconds):
            return
        session = connect(env)
        put(session, f'{APP}/stop-game.txt', '1\n')
        session.quit()
        print(f'stop requested after {args.seconds:.0f} s', flush=True)

    workers = [threading.Thread(target=presses), threading.Thread(target=stop)]
    for worker in workers:
        worker.start()
    klog = out / f'{args.label}-klog.txt'
    run = subprocess.run([sys.executable, 'tools/run-title.py', 'PPSA99008', '--until', UNTIL,
                          '--timeout', str(args.seconds + 120), '--output', str(klog), '--exit-grace', '0',
                          '--echo', r'ProsperoEden\] (launch|shutdown|session failed)|fatal|reason:'],
                         cwd=VULKAN, text=True, capture_output=True)
    done.set()
    for worker in workers:
        worker.join()
    print(run.stdout[-3000:], run.stderr[-2000:], sep='')

    client = connect(env)
    for name in ('heap.log', 'stderr.log'):
        (out / f'{args.label}-{name.replace(".log", ".txt")}').write_text(get(client, f'{DATA}/logs/{name}') or '')
    # Leave the console as it was: the player's resolution, no development files.
    config = json.loads(get(client, f'{DATA}/config/prosperoeden.json') or '{}')
    if chosen_resolution:
        config.setdefault('video', {})['resolution'] = chosen_resolution
        put(client, f'{DATA}/config/prosperoeden.json', json.dumps(config, indent=2) + '\n')
    for name in ('dev-settings.txt', 'compat-input.txt', 'pc-sample.txt', 'stop-game.txt'):
        delete(client, f'{APP}/{name}')
    client.quit()
    heap = (out / f'{args.label}-heap.txt').read_text()
    print(f'{args.label}: {summarise(heap, args.min_draws)}')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Iterative code discovery.

Static analysis cannot see targets of computed jumps.  This script runs the
recompiled game headless with pseudo-random button presses, collects the ROM
addresses that fell back to the interpreter (the `--misses` file), feeds them
back to the lifter as extra roots, rebuilds, and repeats until a run finds
nothing new.  Anything it still misses keeps working (it just runs in the
interpreter), so this is purely an optimisation / coverage step.
"""
import argparse, os, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('rom')
ap.add_argument('--gen', default='generated')
ap.add_argument('--build', default='make')
ap.add_argument('--frames', type=int, default=6000)
ap.add_argument('--seeds', type=int, default=6)
ap.add_argument('--max-iters', type=int, default=6)
a = ap.parse_args()

roots_path = os.path.join(a.gen, 'roots.txt')
exe = os.path.join(os.environ.get('BUILD', 'build'), 'gb')
known = set()
if os.path.exists(roots_path):
    known = {int(t, 16) for t in open(roots_path).read().split()}

for it in range(a.max_iters):
    subprocess.run(a.build.split(), check=True, stdout=subprocess.DEVNULL)
    found = set()
    for seed in range(1, a.seeds + 1):
        mp = os.path.join(a.gen, f'misses_{seed}.txt')
        subprocess.run([exe, '--headless', '--frames', str(a.frames), '--fuzz', str(seed), '--misses', mp, a.rom],
                       check=True, stderr=subprocess.DEVNULL)
        found |= {int(t, 16) for t in open(mp).read().split()}
        os.remove(mp)
    new = found - known
    print(f'iteration {it + 1}: {len(found)} ROM addresses outside recompiled code, {len(new)} new')
    if not new:
        break
    known |= found
    with open(roots_path, 'w') as f:
        f.write('\n'.join(f'{x:04X}' for x in sorted(known)) + '\n')
print('done; roots in', roots_path)

#!/usr/bin/env python3
"""Compile the KisakCOD MP client (minus web/tools/exclude.txt) to wasm32 objects and optionally link a target.

usage: build.py [-j N] [--force] [--only substr] [--link m0]
"""
import argparse, os, re, subprocess, sys, shlex
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
WEB = os.path.join(ROOT, 'web')
BUILD = os.path.join(WEB, 'build'); OBJ = os.path.join(BUILD, 'obj')

DEFS = ['-DWIN32', '-D_CONSOLE', '-D_MBCS', '-DKISAK_MP', '-DCINEMA', '-DUSE_SEPARATE_BLIT_TEXTURE', '-DKISAK_OPENAL',
        '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-DKISAK_WEB']
INCS = ['-I' + os.path.join(WEB, 'shim'), '-I' + os.path.join(ROOT, 'src'), '-I' + os.path.join(ROOT, 'deps'),
        '-I' + os.path.join(ROOT, 'deps', 'msslib')]
COMMON = ['-O1', '-g0', '-pthread', '-fwasm-exceptions', '-w', '-ferror-limit=0'] + DEFS + INCS
CXX = ['em++', '-std=c++20', '-fms-extensions', '-fdelayed-template-parsing', '-msse', '-msimd128', '-Wno-c++11-narrowing',
       '-include', os.path.join(WEB, 'shim', 'kisak_compat.h')] + COMMON
CC = ['emcc', '-std=gnu11'] + COMMON

def excluded():
    ex = set()
    for line in open(os.path.join(HERE, 'exclude.txt')):
        line = line.split('#')[0].strip()
        if line: ex.add(line)
    return ex

def sources(target=None):
    out = subprocess.check_output([sys.executable, os.path.join(HERE, 'filelist.py')], text=True)
    ex = excluded(); res = []
    for line in out.splitlines():
        g, f = line.split('\t')
        if f in ex: continue
        res.append(f)
    # extra TUs that exist only for the web build
    res.append('web/shim/win32_shim.cpp')
    res.append('web/shim/kvfs.cpp')
    if target != 'm0':
        gd = os.path.join(WEB, 'gfx')
        res += ['web/gfx/' + f for f in sorted(os.listdir(gd)) if f.endswith('.cpp')]
    if target:
        d = os.path.join(WEB, target)
        res += ['web/%s/%s' % (target, f) for f in sorted(os.listdir(d)) if f.endswith(('.cpp', '.c'))]
    for f in sorted(os.listdir(os.path.join(WEB, 'platform'))) if os.path.isdir(os.path.join(WEB, 'platform')) else []:
        if f.endswith(('.cpp', '.c')): res.append('web/platform/' + f)
    return res

def objpath(f): return os.path.join(OBJ, f.replace('/', '__') + '.o')

def compile_one(args):
    f, force = args
    src = os.path.join(ROOT, f); obj = objpath(f)
    if not force and os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src):
        return f, 0, '', True
    extra = ['-DDR_WAV_IMPLEMENTATION', '-DDR_MP3_IMPLEMENTATION'] if f.endswith('sound/snd_driver_load_obj.cpp') else []
    cmd = (CXX if f.endswith(('.cpp', '.cc')) else CC) + extra + ['-c', src, '-o', obj]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0 and os.path.exists(obj): os.remove(obj)
    return f, p.returncode, p.stderr, False

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-j', type=int, default=os.cpu_count() or 4); ap.add_argument('--force', action='store_true')
    ap.add_argument('--only'); ap.add_argument('--link')
    args = ap.parse_args()
    os.makedirs(OBJ, exist_ok=True)
    srcs = [f for f in sources(args.link) if not args.only or args.only in f]
    fails = []; built = 0; skipped = 0
    with ThreadPoolExecutor(args.j) as ex:
        for f, rc, err, skip in ex.map(compile_one, [(f, args.force) for f in srcs]):
            if skip: skipped += 1; continue
            if rc: fails.append((f, err)); print('FAIL', f)
            else: built += 1
    print(f'compiled {built}, up-to-date {skipped}, failed {len(fails)} of {len(srcs)}')
    for f, err in fails[:12]:
        print('=====', f)
        print('\n'.join([l for l in err.splitlines() if 'error' in l][:6]))
    if fails: sys.exit(1)
    if args.link: link(args.link, srcs)

def link(target, srcs):
    sys.path.insert(0, HERE)
    import link_targets
    link_targets.link(target, [objpath(f) for f in srcs], ROOT, WEB, BUILD)

main()

#!/usr/bin/env python3
"""Syntax-check every MP-client source file with em++ and aggregate the errors.

usage: survey.py [-j N] [--group GROUP] [--file substr] [--top N] [--show-file]
Logs land in web/build/survey/<file>.log
"""
import argparse, os, re, subprocess, sys, collections
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(ROOT, 'web', 'build', 'survey')

FLAGS = [
    '-std=c++20', '-fms-extensions', '-fdelayed-template-parsing', '-msse', '-msimd128', '-fsyntax-only', '-ferror-limit=0', '-w', '-Wno-c++11-narrowing',
    '-DWIN32', '-D_CONSOLE', '-D_MBCS', '-DKISAK_MP', '-DCINEMA', '-DUSE_SEPARATE_BLIT_TEXTURE',
    '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE',
    '-I' + os.path.join(ROOT, 'web', 'shim'), '-I' + os.path.join(ROOT, 'src'), '-I' + os.path.join(ROOT, 'deps'),
    '-I' + os.path.join(ROOT, 'deps', 'msslib'),
    '-include', os.path.join(ROOT, 'web', 'shim', 'kisak_compat.h'),
]

def files(args):
    out = subprocess.check_output([sys.executable, os.path.join(HERE, 'filelist.py')], text=True)
    res = []
    for line in out.splitlines():
        g, f = line.split('\t')
        if args.group and g != args.group: continue
        if args.file and args.file not in f: continue
        res.append((g, f))
    return res

def run(item):
    g, f = item
    cmd = ['em++'] + FLAGS + [os.path.join(ROOT, f)] if f.endswith(('.cpp', '.cc')) else \
          ['emcc'] + [x for x in FLAGS if not x.startswith('-std=c++')] + ['-std=gnu11', os.path.join(ROOT, f)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    log = os.path.join(OUT, f.replace('/', '__') + '.log')
    open(log, 'w').write(p.stderr)
    return g, f, p.returncode, p.stderr

def norm(msg):
    msg = re.sub(r"'[^']*'", "'X'", msg)
    msg = re.sub(r'"[^"]*"', '"X"', msg)
    return msg

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-j', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--group'); ap.add_argument('--file'); ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--raw', action='store_true', help='group by raw (un-normalised) message')
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    items = files(args)
    ok = 0; bad = []
    msgs = collections.Counter(); msg_files = collections.defaultdict(set); first_err = {}
    with ThreadPoolExecutor(args.j) as ex:
        for g, f, rc, err in ex.map(run, items):
            if rc == 0: ok += 1
            else: bad.append(f)
            for m in re.finditer(r'^(\S+?):(\d+):\d+: (fatal error|error): (.*)$', err, re.M):
                key = m.group(4) if args.raw else norm(m.group(4))
                msgs[key] += 1; msg_files[key].add(f)
                first_err.setdefault(key, f'{os.path.relpath(m.group(1), ROOT)}:{m.group(2)}')
    print(f'{ok}/{len(items)} files compile clean; {len(bad)} with errors; {sum(msgs.values())} total errors')
    for key, n in msgs.most_common(args.top):
        print(f'{n:6d} err in {len(msg_files[key]):3d} files  {key[:150]}\n          e.g. {first_err[key]}')

main()

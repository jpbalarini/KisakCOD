#!/usr/bin/env python3
"""Extract the MP client source file list from KisakCOD's cmake file lists."""
import re, sys, os
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SRC = os.path.join(ROOT, 'src'); DEPS = os.path.join(ROOT, 'deps')

def parse(path):
    txt = open(path).read()
    sets = {}
    for m in re.finditer(r'set\(\s*(\w+)\s*(.*?)\)', txt, re.S):
        name, body = m.group(1), m.group(2)
        files = re.findall(r'"([^"]+)"', body)
        sets.setdefault(name, []).extend(files)
    return sets

def resolve(f):
    return f.replace('${SRC_DIR}', SRC).replace('${DEPS_DIR}', DEPS)

def main():
    common = parse(os.path.join(ROOT, 'scripts', 'common_files.cmake'))
    mp = parse(os.path.join(ROOT, 'scripts', 'mp', 'mp_files.cmake'))
    allsets = {}
    for d in (common, mp):
        for k, v in d.items(): allsets.setdefault(k, []).extend(v)
    # groups used by the MP CMakeLists add_executable()
    groups = ['SRCROOT','DYNENTITY','EFFECTSCORE','AIM_ASSIST','BGAME','BINKLIB','CGAME','CGAME_MP','CLIENT','CLIENT_MP',
              'COMMON','DATABASE','DEVGUI','GAME','GAME_MP','GFX_D3D','GROUPVOICE','GROUPVOICE_SPEEX','MSSLIB','ODE','PHYSICS',
              'PHYSICS_ODE','QCOMMON','RAGDOLL','SCRIPT','SERVER','SERVER_MP','SOUND','SOUND_BACKEND','SPEEX','STRINGED','UI',
              'UI_MP','UNIVERSAL','WIN32_SRC','XANIM','ZLIB']
    out = []
    for g in groups:
        for f in allsets.get(g, []):
            f = resolve(f)
            if f.endswith(('.cpp', '.c', '.cc')):
                out.append((g, os.path.relpath(f, ROOT)))
    if len(sys.argv) > 1 and sys.argv[1] == '--groups':
        from collections import Counter
        c = Counter(g for g, _ in out)
        for g in groups: print(f'{c.get(g,0):5d} {g}')
        print(sum(c.values()), 'total'); return
    for g, f in out: print(f'{g}\t{f}')

main()

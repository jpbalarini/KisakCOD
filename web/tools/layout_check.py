#!/usr/bin/env python3
"""layout_check.py -- verify struct sizes of the wasm32/clang build against the sizes recorded in the decompiled headers.

The engine headers are annotated with the retail 32-bit MSVC size of each struct (`struct foo_t // sizeof=0x1C5B0`). The fastfile loader,
network protocol and a lot of decompiled pointer arithmetic depend on those layouts, so any struct that comes out a different size under
clang/wasm32 is a candidate for the same class of bug as the MSVC `rand()` / union-by-value / adjacent-globals problems found in M2/M3.

For every header with annotated structs this generates a tiny TU that includes it and `static_assert`s each size, compiles it with the
engine flags (syntax only), and reports the mismatches. Known/explained differences are listed in EXPECTED below; anything else is a
failure (exit status 1).

    python3 web/tools/layout_check.py
"""
import os, re, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.dirname(HERE)
ROOT = os.path.dirname(WEB)
SRC = os.path.join(ROOT, 'src')
sys.path.insert(0, HERE)
import build  # noqa: E402  (reuses the engine compile flags)

# (header, struct) -> why the clang/wasm size differs and why that is harmless.
EXPECTED = {
    ('bgame/bg_public.h', 'missile_ent_t'): 'SP variant of an MP/SP #if pair (the MP one has its own static_assert)',
    ('bgame/bg_local.h', 'CEntVehicleInfo'): 'SP variant of an MP/SP #if pair (the MP one has its own static_assert)',
    ('bgame/bg_local.h', 'scr_vehicle_s'): 'SP variant of an MP/SP #if pair (the MP one has its own static_assert)',
    ('game/game_public.h', 'vehicle_info_t'): 'SP-only data (no vehicles in the MP maps)',
    ('ui/ui.h', 'sharedUiInfo_t'): 'SP-only header (#error under KISAK_MP); ui_mp/ui_mp.h has the MP one',
    ('gfx_d3d/r_gfx.h', 'GfxWorldStreamInfo'): 'empty struct: 0 in the IDA annotation, 1 in C++',
    ('stringed/stringed_ingame.h', 'LocalizeName'): 'empty struct: 0 in the IDA annotation, 1 in C++',
    ('xanim/xmodel.h', 'XModelStreamInfo'): 'empty struct: 0 in the IDA annotation, 1 in C++',
    ('stringed/stringed_ingame.h', 'CStringEdPackage'): 'holds std containers (MSVC STL vs libc++ sizes); only used through its own methods',
    ('qcommon/net_chan_mp.h', 'loopback_t'): 'buffers deliberately enlarged by the KisakCOD authors',
    ('qcommon/net_chan_mp.h', 'loopmsg_t'): 'buffers deliberately enlarged by the KisakCOD authors',
    ('client_mp/client_mp.h', 'VoicePacket_t'): 'retail is packed to 0x105; the reconstructed header is naturally aligned (0x108) on MSVC too. '
                                                'Only the voice path and the client_t size are affected (SV_Set/GetClientStat no longer rely on it)',
    ('server_mp/server_mp.h', 'client_t'): '40 x VoicePacket_t (see above), +120 bytes',
    ('physics/phys_local.h', 'PhysGlob'): 'ODE joint arrays; internal to physics, no hard-coded offsets reach into it',
    ('physics/ode/odeext.h', 'odeGlob_t'): 'ODE bodies/joints; internal to physics, no hard-coded offsets reach into it',
}

PAT = re.compile(r'^\s*(?:typedef\s+)?(?:struct|union|class)\s+(?:__declspec\([^)]*\)\s*)?([A-Za-z_]\w*)\s*(?://|/\*)\s*sizeof\s*=\s*(0x[0-9A-Fa-f]+|\d+)')


def collect():
    hdrs = {}
    for dp, _, fn in os.walk(SRC):
        for f in fn:
            if not f.endswith('.h'): continue
            p = os.path.join(dp, f)
            for ln in open(p, errors='replace'):
                m = PAT.match(ln)
                if m: hdrs.setdefault(os.path.relpath(p, SRC), []).append((m.group(1), int(m.group(2), 0)))
    return hdrs


def main():
    hdrs = collect()
    tmp = tempfile.mkdtemp(prefix='layout_check_')
    cmd = [c for c in build.CXX if c not in ('-O1', '-g0')] + ['-fsyntax-only', '-w']

    def run(item):
        rel, structs = item
        tu = os.path.join(tmp, rel.replace('/', '__') + '.cpp')
        with open(tu, 'w') as f:
            f.write('#include <universal/q_shared.h>\n#include <Windows.h>\n#include <%s>\n' % rel)
            for n, s in structs:
                f.write('static_assert(sizeof(%s) == %d, "LAYOUT %s %s %d");\n' % (n, s, rel, n, s))
        r = subprocess.run(cmd + [tu], capture_output=True, text=True)
        return rel, r.stderr

    bad, unexpected = [], []
    with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        for rel, err in ex.map(run, sorted(hdrs.items())):
            lines = err.splitlines()
            for i, l in enumerate(lines):
                m = re.search(r'static assertion failed.*LAYOUT (\S+) (\S+) (\d+)', l)
                if not m: continue
                got = next((mm.group(1) for l2 in lines[i + 1:i + 4] for mm in [re.search(r"evaluates to '(\d+) == \d+'", l2)] if mm), '?')
                bad.append((rel, m.group(2), int(m.group(3)), got))

    total = sum(len(v) for v in hdrs.values())
    print('checked %d annotated structs in %d headers; %d differ' % (total, len(hdrs), len(bad)))
    for rel, name, exp, got in bad:
        why = EXPECTED.get((rel, name))
        print('  %-9s %s::%s expected %d got %s%s' % ('expected' if why else 'UNEXPECTED', rel, name, exp, got, ('  -- ' + why) if why else ''))
        if not why: unexpected.append(name)
    return 1 if unexpected else 0


if __name__ == '__main__':
    sys.exit(main())

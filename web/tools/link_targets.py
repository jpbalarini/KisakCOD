import os, subprocess, sys

def link_app(target, objs, root, web, build):
    out_dir = os.path.join(build, target); os.makedirs(out_dir, exist_ok=True)
    cmd = ['em++', '-pthread', '-fwasm-exceptions', '-O1', '-g0'] + objs + [
        '-o', os.path.join(out_dir, 'index.html'), '--shell-file', os.path.join(web, 'app', 'shell.html'),
        '-sPTHREAD_POOL_SIZE=8', '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=536870912', '-sMAXIMUM_MEMORY=2147483648',
        '-sSTACK_SIZE=8388608', '-sDEFAULT_PTHREAD_STACK_SIZE=4194304', '-sSUPPORT_LONGJMP=wasm',
        '-sEMULATE_FUNCTION_POINTER_CASTS=1', '--profiling-funcs', '-sERROR_ON_UNDEFINED_SYMBOLS=0', '-sWARN_ON_UNDEFINED_SYMBOLS=1',
        '-sENVIRONMENT=web,worker', '-sEXIT_RUNTIME=0', '-sASSERTIONS=1',
        '--pre-js', os.path.join(web, 'app', 'prejs.js'), '-sPROXY_TO_PTHREAD', '-sOFFSCREENCANVAS_SUPPORT', '-sOFFSCREEN_FRAMEBUFFER', '-sMIN_WEBGL_VERSION=2', '-sMAX_WEBGL_VERSION=2']
    p = subprocess.run(cmd, capture_output=True, text=True)
    open(os.path.join(out_dir, 'link.log'), 'w').write(p.stdout + p.stderr)
    import shutil
    for aux in ('audio-worklet.js', 'io-worker.js', 'hero.webp'): shutil.copy(os.path.join(web, 'app', aux), out_dir)
    jsf = os.path.join(out_dir, 'index.js')
    if os.path.exists(jsf):
        js = open(jsf).read().replace('captured an uncaught exception: ${ex}`', 'captured an uncaught exception: ${ex}\\n${ex && ex.stack}`')
        open(jsf, 'w').write(js)
    und = sorted(set(l.split('undefined symbol: ')[1].split(' ')[0] for l in p.stderr.splitlines() if 'undefined symbol: ' in l))
    open(os.path.join(out_dir, 'undefined.txt'), 'w').write('\n'.join(und) + '\n')
    errs = [l for l in p.stderr.splitlines() if 'error' in l.lower()]
    print(f'link rc={p.returncode}; {len(und)} undefined symbols (web/build/{target}/undefined.txt); {len(errs)} error lines')
    for l in errs[:25]: print('  ', l[:220])
    if p.returncode: sys.exit(p.returncode)

def link(target, objs, root, web, build):
    if target == 'app': return link_app(target, objs, root, web, build)
    if target != 'loadtest': raise SystemExit('unknown link target ' + target)
    out_dir = os.path.join(build, target); os.makedirs(out_dir, exist_ok=True)
    extra_objs = []
    cmd = ['em++', '-pthread', '-fwasm-exceptions', '-O1', '-g0'] + objs + extra_objs + [
        '-o', os.path.join(out_dir, 'loadtest.js'),
        '-sPTHREAD_POOL_SIZE=6', '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=268435456', '-sMAXIMUM_MEMORY=2147483648',
        '-sSTACK_SIZE=8388608', '-sDEFAULT_PTHREAD_STACK_SIZE=4194304', '-sSUPPORT_LONGJMP=wasm',
        '-sEMULATE_FUNCTION_POINTER_CASTS=1', '--profiling-funcs', '-sERROR_ON_UNDEFINED_SYMBOLS=0', '-sWARN_ON_UNDEFINED_SYMBOLS=1',
        '-sENVIRONMENT=node', '-sNODERAWFS=1', '-sEXIT_RUNTIME=1', '-sASSERTIONS=1']
    p = subprocess.run(cmd, capture_output=True, text=True)
    open(os.path.join(out_dir, 'link.log'), 'w').write(p.stdout + p.stderr)
    # debugging aid: print the wasm stack for uncaught exceptions on pthread workers
    jsf = os.path.join(out_dir, 'loadtest.js')
    if os.path.exists(jsf):
        js = open(jsf).read().replace('captured an uncaught exception: ${ex}`', 'captured an uncaught exception: ${ex}\\n${ex && ex.stack}`')
        open(jsf, 'w').write(js)
    und = sorted(set(l.split('undefined symbol: ')[1].split(' ')[0] for l in p.stderr.splitlines() if 'undefined symbol: ' in l))
    open(os.path.join(out_dir, 'undefined.txt'), 'w').write('\n'.join(und) + '\n')
    errs = [l for l in p.stderr.splitlines() if 'error' in l.lower()]
    print(f'link rc={p.returncode}; {len(und)} undefined symbols (web/build/{target}/undefined.txt); {len(errs)} error lines')
    for l in errs[:25]: print('  ', l[:220])
    if p.returncode: sys.exit(p.returncode)

"""PS1 reference (DuckStation trace.txt + audio.wav) vs OpenCross (OC_SFX_TRACE=1) sound comparison.
  sfx_cmp.py ps1 <dir>            <dir> from tools/py/ps1_audio.py (trace.txt, audio.wav, action.sav) -> <dir>/events.csv, prints the table + audio onsets
  sfx_cmp.py ours <log> [out.csv] normalise an opencross run (REQ + START lines) to the same columns
  sfx_cmp.py cmp <ps1.csv> <ours.csv> [--align TAG]   missing / extra / timing / pan / volume differences, aligned by the first common tag
columns: t_s, kind (menu|snd), tag, mask, pan (0..255, 0x80 centre), vol (0..127), ra (PS1 caller), path (menu | anm33 | anm7B | tag800368c4 | ovl:<ra> | ours)
Our menu ids 'gN' (decimal) equal the PS1 Sound_PlaySfx ids '#hex'."""
import sys, os, re, csv, glob, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def tagstr(v):
    b = struct.pack('<I', v & 0xFFFFFFFF).rstrip(b'\0')
    return b.decode('latin1') if b and all(32 <= c < 127 for c in b) else '%X' % v


def resmap(statedir):
    """file pointer -> tag for every type-0x16 node of the global resource list (union over the states)"""
    import ps1_state as c
    m = {}
    for p in sorted(glob.glob(os.path.join(statedir, '*.sav'))):
        try:
            st = c.State(p)
        except Exception:
            continue
        e = st.u32(st.map.effect_registry)
        n = 0
        while e and n < 4000:
            n += 1
            if (st.u32(e + 8) & 0x3F) == 0x16:
                m.setdefault(st.u32(e + 0xC) if (st.u8(e + 8) & 0x80) else e + 0xC, tagstr(st.u32(e + 4)))
            e = st.u32(e)
            if not (0x80000000 <= e < 0x82000000):
                break
    return m


def trace_events(d):
    """trace.txt of a ps1_audio.py folder -> (pc, t_s, ra, a0, a1, a2, a3); t_s = audio time (frames after the action state / 60)."""
    action = 0
    mp = os.path.join(d, 'manifest.txt')
    if os.path.exists(mp):
        for ln in open(mp):
            if ln.startswith('action_frame '):
                action = int(ln.split()[1])
    for ln in open(os.path.join(d, 'trace.txt'), errors='replace'):
        f = ln.split()
        if len(f) < 7: continue
        yield f[1].upper().rjust(8, '0'), (int(f[0]) - action) / 60.0, f[6].upper().rjust(8, '0'), int(f[2], 16), int(f[3], 16), int(f[4], 16), int(f[5], 16)

def wavfile(d):
    return os.path.join(d, 'audio.wav') if os.path.exists(os.path.join(d, 'audio.wav')) else os.path.join(d, 'audio.raw')

L = re.compile(r'TRACEPC ([0-9A-F]{8}) t=([0-9.]+) ra=([0-9A-F]{8}) a0=([0-9A-F]+) a1=([0-9A-F]+) a2=([0-9A-F]+) a3=([0-9A-F]+)')


def ps1(d):
    rm = resmap(d)
    ev = []
    cur = None  # (path, ra, t[, tag])
    for pc, t, ra, a0, a1, a2, a3 in trace_events(d):
        if pc == '8004A118':
            ev.append(dict(t=t, kind='menu', tag='#%X' % a0, mask=a1, pan=a2, vol=a3, ra=ra, path='menu'))
        elif pc == '800B9DDC':
            cur = ('anm33', ra, t)
        elif pc == '800B9F90':
            cur = ('anm7B', ra, t)
        elif pc == '800368C4':
            cur = ('tag800368c4', ra, t, tagstr(a0))
        elif pc == '8004A168':
            tag = rm.get(a0, 'ptr%X' % a0)
            if cur and abs(cur[2] - t) < 1e-6:
                path, cra = cur[0], cur[1]
                if path == 'tag800368c4':
                    tag = cur[3]
            else:
                path, cra = 'ovl:' + ra, ra
            ev.append(dict(t=t, kind='snd', tag=tag, mask=a1, pan=a2, vol=a3, ra=cra, path=path))
    write(os.path.join(d, 'events.csv'), ev)
    return ev


def write(p, ev):
    with open(p, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['t_s', 'kind', 'tag', 'mask', 'pan', 'vol', 'ra', 'path'])
        for e in ev:
            w.writerow(['%.3f' % e['t'], e['kind'], e['tag'], '%X' % e['mask'], e['pan'], e['vol'], e['ra'], e['path']])


def read(p):
    return [dict(t=float(r['t_s']), kind=r['kind'], tag=r['tag'], mask=int(r['mask'], 16), pan=int(r['pan']), vol=int(r['vol']), ra=r['ra'], path=r['path'])
            for r in csv.DictReader(open(p))]


def onsets(d):
    import x2_audio as a
    return a.events(a.load_wav(os.path.join(d, 'audio.wav'))[0] if os.path.exists(os.path.join(d, 'audio.wav')) else a.load(os.path.join(d, 'audio.raw')), thr=0.004, gap=0.25)


def report(d):
    ev = ps1(d)
    print('%d events' % len(ev))
    for e in ev:
        print('%8.3f %-5s %-8s mask=%-8X pan=%-3d vol=%-3d %-12s ra=%s' % (e['t'], e['kind'], e['tag'], e['mask'], e['pan'], e['vol'], e['path'], e['ra']))
    on = onsets(d)
    print('audible regions (s):', ', '.join('%.2f-%.2f' % o for o in on))
    for e in ev:
        if not any(o[0] - 0.05 <= e['t'] + 0.5 and o[1] >= e['t'] - 0.01 for o in on):
            print('  event with no audible onset near it: %.3f %s' % (e['t'], e['tag']))
    for o in on:
        if not any(e['t'] - 0.3 <= o[0] <= e['t'] + 0.3 or (e['t'] <= o[0] and o[1] >= e['t']) for e in ev):
            print('  audio region with no event: %.2f-%.2f (music / un-hooked path)' % o)


def ours(p, out=None):
    pend = []
    for ln in open(p, errors='replace'):
        m = re.search(r"sfxtrace\s+([\d.]+) ms REQ '([^']*)' mask (\S+)", ln)
        if m:
            tag = m.group(2)
            isg = re.fullmatch(r'g\d+', tag) is not None
            pend.append(dict(t=float(m.group(1)) / 1000, kind='menu' if isg else 'snd', tag=('#%X' % int(tag[1:])) if isg else tag,
                             mask=int(m.group(3), 16), pan=-1, vol=-1, ra='', path='ours', got=False))
            continue
        m = re.search(r"sfxtrace\s+([\d.]+) ms START #\d+ '([^']*)'(?: ch \d+)? pc \S+ mask \S+ vol (\S+) pan (\S+)", ln)
        if m:
            tg = m.group(2)
            tag = ('#%X' % int(tg[1:])) if re.fullmatch(r'g\d+', tg) else tg
            for e in pend:
                if not e['got'] and e['tag'] == tag:
                    e['got'] = True
                    e['vol'] = int(round(float(m.group(3)) * 127))
                    pn = float(m.group(4))
                    e['pan'] = 0x80 if pn < 0 else int(pn) * 2
                    break
    ev = [{k: v for k, v in e.items() if k != 'got'} for e in pend]
    # one request per entry file: collapse same tag + same time (a multi-entry file is requested once per entry)
    ev = [e for i, e in enumerate(ev) if not any(o['tag'] == e['tag'] and o['t'] == e['t'] for o in ev[:i])]
    if out:
        write(out, ev)
    return ev


def cmpf(a, b, align=None):
    pa, ob = read(a), read(b)
    tags = [e['tag'] for e in pa]
    base = align or next((o['tag'] for o in ob if o['tag'] in tags), None)
    if not base:
        print('no common tag')
        return
    sh = next(e['t'] for e in pa if e['tag'] == base) - next(o['t'] for o in ob if o['tag'] == base)
    print('aligned on first %r: ours + %.3f s' % (base, sh))
    used = set()
    for e in pa:
        best = next((j for j, o in enumerate(ob) if j not in used and o['tag'] == e['tag'] and abs(o['t'] + sh - e['t']) < 1.5), None)
        if best is None:
            print('MISSING in ours: %7.3f %-8s pan=%d vol=%d %s ra=%s' % (e['t'], e['tag'], e['pan'], e['vol'], e['path'], e['ra']))
            continue
        used.add(best)
        o = ob[best]
        flag = '' if abs(o['t'] + sh - e['t']) < 0.05 else '  <- TIMING'
        if e['kind'] == 'snd' and (abs(o['pan'] - e['pan']) > 12 or abs(o['vol'] - e['vol']) > 12):
            flag += '  <- LEVEL'
        print('both %-8s ps1 %7.3f ours %7.3f d=%+6.3f | pan %3d/%3d vol %3d/%3d | %s%s' % (e['tag'], e['t'], o['t'] + sh, o['t'] + sh - e['t'], e['pan'], o['pan'], e['vol'], o['vol'], e['path'], flag))
    for j, o in enumerate(ob):
        if j not in used:
            print('EXTRA in ours:   %7.3f %-8s mask=%X pan=%d vol=%d' % (o['t'] + sh, o['tag'], o['mask'], o['pan'], o['vol']))


if __name__ == '__main__':
    c = sys.argv[1]
    if c == 'ps1':
        report(sys.argv[2])
    elif c == 'ours':
        for e in ours(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None):
            print('%8.3f %-6s %-8s mask=%X pan=%d vol=%d' % (e['t'], e['kind'], e['tag'], e['mask'], e['pan'], e['vol']))
    elif c == 'cmp':
        cmpf(sys.argv[2], sys.argv[3], sys.argv[sys.argv.index('--align') + 1] if '--align' in sys.argv else None)

r"""PS1 reference (DuckStation trace.txt + audio.wav) vs OpenCross (OC_SFX_TRACE) sound-request comparison.
  sfx_cmp2.py ps1  <dir>            -> <dir>/events.csv  (t_s,kind,tag,mask,pan,vol,ra,path) + onset report; <dir> from tools/py/ps1_audio.py (trace.txt, audio.wav, action.sav)
  sfx_cmp2.py ours <log> [<out.csv>] -> normalised events of an opencross run (OC_SFX_TRACE=1 stdout/log)
  sfx_cmp2.py cmp  <ps1 events.csv> <ours events.csv> [--shift S]   -> missing / extra / timing / pan / vol differences
kinds: menu (Sound_PlaySfx id, tag = '#<hex id>'), snd (Sound_PlaySfxFromPointer; tag resolved from the global resource list of a state)
paths: anm33 / anm7B (anmseq op), fx4C (effect sequence op 0x4C, mask/pan/vol are its operands), tag800368c4:<caller ra>, ovl:<ra>"""
import sys, os, re, csv, glob, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

def tagstr(v):
    b = struct.pack('<I', v & 0xFFFFFFFF).rstrip(b'\0')
    return b.decode('latin1') if b and all(32 <= c < 127 for c in b) else '%X' % v

def resmap(statedir):
    """data pointer -> tag for every type-0x16 entry of the global resource list in the given states (union)"""
    import ps1_state as c
    m = {}
    for p in sorted(glob.glob(os.path.join(statedir, '*.sav'))):
        try:
            st = c.State(p)
        except Exception:
            continue
        e = st.u32(st.map.effect_registry); n = 0
        while e and n < 4000:
            n += 1
            if (st.u32(e + 8) & 0x3F) == 0x16:
                data = st.u32(e + 0xC) if (st.u8(e + 8) & 0x80) else e + 0xC
                m.setdefault(data, tagstr(st.u32(e + 4)))
            e = st.u32(e)
            if not (0x80000000 <= e < 0x80200000): break
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
    ev = []; last = None
    for pc, t, ra, a0, a1, a2, a3 in trace_events(d):
        if pc == '8004A118':   # Sound_PlaySfx(id, mask, pan, vol)
            ev.append(dict(t=t, kind='menu', tag='#%X' % a0, mask=a1, pan=a2, vol=a3, ra=ra, path='menu'))
        elif pc == '800368C4':
            last = (t, ra, tagstr(a0), a1, a2, a3)
        elif pc == '8004A168':   # Sound_PlaySfxFromPointer(file, mask, pan, vol)
            if last and abs(last[0] - t) < 1e-6 and ra == '80036918':
                ev.append(dict(t=t, kind='snd', tag=last[2], mask=a1, pan=a2, vol=a3, ra=last[1], path='tag800368c4:' + last[1]))
            else:
                tag = rm.get(a0, 'ptr%X' % a0)
                path = 'anm33' if ra.startswith('800B9F6') or ra == '800B9F64' else ('fx4C' if ra == '8002DC74' else 'ovl:' + ra)
                ev.append(dict(t=t, kind='snd', tag=tag, mask=a1, pan=a2, vol=a3, ra=ra, path=path))
            last = None
    write(os.path.join(d, 'events.csv'), ev)
    return ev

def write(p, ev):
    with open(p, 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['t_s', 'kind', 'tag', 'mask', 'pan', 'vol', 'ra', 'path'])
        for e in ev: w.writerow(['%.3f' % e['t'], e['kind'], e['tag'], '%X' % e['mask'], e['pan'], e['vol'], e['ra'], e['path']])
def read(p):
    r = []
    for row in csv.DictReader(open(p)):
        r.append(dict(t=float(row['t_s']), kind=row['kind'], tag=row['tag'], mask=int(row['mask'], 16), pan=int(row['pan']), vol=int(row['vol']), ra=row['ra'], path=row['path']))
    return r

def onsets(d):
    import x2_audio as a
    x = a.load_wav(os.path.join(d, 'audio.wav'))[0] if os.path.exists(os.path.join(d, 'audio.wav')) else a.load(os.path.join(d, 'audio.raw'))
    return a.events(x, thr=0.004, gap=0.25)

def report(d):
    ev = ps1(d)
    print('%d events' % len(ev))
    for e in ev: print('%8.3f %-5s %-8s mask=%-7X pan=%-3d vol=%-3d %s' % (e['t'], e['kind'], e['tag'], e['mask'], e['pan'], e['vol'], e['path']))
    try:
        on = onsets(d)
    except Exception as ex:
        print('onsets: %s' % ex); return
    print('audible regions (start,end s):', ', '.join('%.2f-%.2f' % o for o in on))
    for e in ev:
        if not any(o[0] - 0.05 <= e['t'] + 0.5 and o[1] >= e['t'] - 0.01 for o in on): print('  event with no audio near it:', e['t'], e['tag'])
    for o in on:
        if not any(e['t'] - 0.3 <= o[0] <= e['t'] + 0.3 or (e['t'] <= o[0] and o[1] >= e['t']) for e in ev): print('  audio region with no event: %.2f-%.2f (music/ambience or an un-hooked path)' % o)

# ---- ours -------------------------------------------------------------------------------------------------------------------
def ours(p, out=None):
    """REQ lines (queue 0 = one per request) with the vol/pan of the first START of the same tag after it (vol 0..1 -> 0..127, pan 0..127 -> 0..255)"""
    ev = []; pend = []; seen_req = set()
    for ln in open(p, errors='replace'):
        m = re.search(r"sfxtrace\s+([0-9.]+) ms REQ '([^']*)' mask (\S+) programs \d+ \(queue (\d+)\)", ln)
        if m:
            tt = float(m.group(1)) / 1000
            if not any(e['tag'] == m.group(2) and abs(e['t'] - tt) < 0.2 for e in ev[-12:]):   # the entries of a multi-entry file queue on consecutive ticks: one event per tag
                e = dict(t=float(m.group(1)) / 1000, kind='snd', tag=m.group(2), mask=int(m.group(3), 16), pan=128, vol=127, ra='', path='ours', got=False)
                ev.append(e); pend.append(e)
            continue
        m = re.search(r"sfxtrace\s+([0-9.]+) ms START #\d+ '([^']*)'(?: ch \d+)? pc \S+ mask \S+ vol (\S+) pan (\S+)", ln)
        if m:
            for e in pend:
                if e['tag'] == m.group(2) and not e['got']:
                    e['got'] = True; e['vol'] = int(round(float(m.group(3)) * 127))
                    pn = int(m.group(4)); e['pan'] = 128 if pn < 0 else pn * 2
                    break
    for e in ev: e.pop('got', None)
    if out: write(out, ev)
    return ev

def cmpf(a, b, shift=None):
    pa, ob = read(a), read(b)
    pa = [e for e in pa if e['kind'] == 'snd']; ob = [e for e in ob if e['kind'] == 'snd']
    if shift is None:   # align on the first tag both sides have
        shift = 0.0
        for e in pa:
            o = next((o for o in ob if o['tag'] == e['tag']), None)
            if o: shift = e['t'] - o['t']; print('aligned on %s: ours t + %.3f' % (e['tag'], shift)); break
    used = set()
    for e in pa:
        best = None
        for j, o in enumerate(ob):
            if j in used or o['tag'] != e['tag']: continue
            best = j; break
        if best is None: print('MISSING in ours: %7.3f %-6s pan=%d vol=%d %s' % (e['t'], e['tag'], e['pan'], e['vol'], e['path'])); continue
        used.add(best); o = ob[best]
        print('both %-6s ps1 t=%7.3f ours t=%7.3f d=%+6.3f | pan %3d/%3d vol %3d/%3d | %s' % (e['tag'], e['t'], o['t'] + shift, o['t'] + shift - e['t'], e['pan'], o['pan'], e['vol'], o['vol'], e['path']))
    for j, o in enumerate(ob):
        if j not in used: print('EXTRA in ours:   %7.3f %-6s pan=%d vol=%d' % (o['t'] + shift, o['tag'], o['pan'], o['vol']))

if __name__ == '__main__':
    c = sys.argv[1]
    if c == 'ps1': report(sys.argv[2])
    elif c == 'ours': ours(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else sys.argv[2] + '.csv')
    elif c == 'cmp': cmpf(sys.argv[2], sys.argv[3], (float(sys.argv[5]) if len(sys.argv) > 5 else None))

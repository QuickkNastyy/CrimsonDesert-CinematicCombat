"""End-to-end check of the control panel, game pause, hook retry, hit-stop and telemetry against the test host (port 6401): real window (parked off-screen,
never takes focus), real UI thread, real XInput hook over a FAKE xinput1_4.dll with a scripted pad, a stand-in time controller. Never touches the real game."""
import json, os, re, struct, subprocess, sys, time, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bridge_client import send
import layout
LA = layout.L   # src/gamelayout.h

H = os.path.abspath(os.path.join(HERE, '..', 'build', 'harness'))
ini = os.path.join(H, 'CombatMod.ini')
S = lambda c: send(c, port=6401)
PAD = os.path.join(H, 'pad.txt')
RESULTS = []


def check(name, cond, detail=''):
    RESULTS.append((name, bool(cond)))
    print('%s %s %s' % ('PASS' if cond else 'FAIL', name, detail))


def pad(buttons=0, lx=0, conn=1):
    tmp = PAD + '.tmp'
    with open(tmp, 'w') as f:
        f.write('%d %d %d' % (buttons, lx, conn))
    os.replace(tmp, PAD)


def kv(text, key):
    m = re.search(key + r'=(\S+)', text)
    return m.group(1) if m else None


def status():
    return S('ui status')


def seen():
    v = int(S('readq %s' % g['padseen']).split('=')[1].split()[0], 16)
    return v & 0xFFFF, bool(v >> 40)


def cfg(key):
    return S('cfg get ' + key).split('=')[1].split()[0]


def rd_double(addr):
    v = int(S('readq %x' % addr).split('=')[1].split()[0], 16)
    return struct.unpack('<d', struct.pack('<Q', v))[0]


def rd_float(addr):
    return float(S('readf %x' % addr).split('=')[1].split()[0])


def sim_rate(seconds=0.6):
    a = rd_double(int(g['simtime'], 16)); t = time.time()
    time.sleep(seconds)
    b = rd_double(int(g['simtime'], 16))
    return (b - a) / (time.time() - t)


def rows():
    out = {}
    btn = []
    for line in S('ui rows').split('\n'):
        m = re.match(r'(\d+) (\S+) (\S+)', line)
        if not m:
            continue
        i, kind, key = int(m.group(1)), m.group(2), m.group(3)
        if kind == 'buttons':
            btn.append(i)
        elif key != '-':
            out[key] = i
    return out, btn


def tap(b, hold=0.12):
    pad(b, 0); time.sleep(hold); pad(0, 0); time.sleep(0.12)


def bmp_to_png(bmp, png):
    d = open(bmp, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    h = abs(h)
    raw = bytearray()
    for y in range(h):
        row = d[off + y * w * 4: off + (y + 1) * w * 4]
        raw.append(0)
        for x in range(w):
            b, g_, r, _ = row[x * 4: x * 4 + 4]
            raw += bytes((r, g_, b))
    def chunk(t, c):
        return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xFFFFFFFF)
    open(png, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))


def sigline(name, addr, nbytes=40, rip=None):
    rd = S('read %s %d' % (addr, nbytes))
    hb = []
    for line in rd.split('\n')[:4]:
        for tok in line.split()[1:]:
            if len(tok) == 2 and all(c in '0123456789ABCDEF' for c in tok):
                hb.append(tok)
    return hb[:nbytes]


txt = open(ini).read()
txt = re.sub(r'\n\[UI\].*?(?=\n\[|\Z)', '', txt, flags=re.S) + '\n[UI]\nEnabled=1\nTestMode=1\nAlwaysPoll=1\nAlpha=240\n'
txt = re.sub(r'\n\[Telemetry\].*?(?=\n\[|\Z)', '', txt, flags=re.S) + '\n[Telemetry]\nEnabled=1\nMaxMB=20\n'
open(ini, 'w').write(txt)
shutil_tel = os.path.join(H, 'CombatMod_telemetry')
if os.path.isdir(shutil_tel):
    for f in os.listdir(shutil_tel):
        os.remove(os.path.join(shutil_tel, f))
# no signatures at start-up: nothing may install a hook before the test arms its failure injection (a sigs file left by a previous run would)
open(os.path.join(H, 'CombatMod.sigs'), 'w').write('# written by the test after start-up' + chr(10))
pad(0, 0, 1)
out = open(os.path.join(H, 'out.txt'), 'w')
p = subprocess.Popen([os.path.join(H, 'CrimsonDesert.exe')], cwd=H, stdout=out, stderr=subprocess.STDOUT, creationflags=0x08000000)
try:
    time.sleep(2)
    # safety: port 6401 must be owned by THE stand-in process we just started, nothing else (never the real game)
    ns = subprocess.run(['netstat', '-ano', '-p', 'tcp'], capture_output=True, text=True).stdout
    owners = {int(l.split()[-1]) for l in ns.splitlines() if ':6401 ' in l and 'LISTENING' in l}
    if owners != {p.pid}:
        p.kill()
        raise SystemExit('ABORT: port 6401 is owned by %s, not by the stand-in host (pid %d)' % (sorted(owners), p.pid))
    g = dict(re.findall(r'(\w+)=([0-9a-f]+)', open(os.path.join(H, 'out.txt')).read().split('\n')[0]))
    # ---- signatures of the stand-in functions (the real game's come from CombatMod.sigs)
    with open(os.path.join(H, 'CombatMod.sigs'), 'w') as f:
        f.write('time_curve_start = %s | off=0 | deref=0\n' % ' '.join(sigline('', g['curve'])))
        f.write('apply_attack_skill_to_position = %s | off=0 | deref=0\n' % ' '.join(sigline('', g['apply'])))
        f.write('event_dispatcher = %s | off=0 | deref=0\n' % ' '.join(sigline('', g['disp'])))
        gb = sigline('', g['getg'], 24)
        check('getg prologue shape', gb[:3] == ['48', '8B', '05'] and gb[7:9] == ['C7', '05'] and gb[13:17] == ['34', '12', 'CD', 'AB'] and gb[17] == 'C3', ' '.join(gb))
        pat = ' '.join(gb[:3] + ['??'] * 4 + gb[7:9] + ['??'] * 4 + gb[13:18])
        f.write('time_mgr_global = %s | off=0 | rip=3 | deref=0\n' % pat)
    print(S('hooktest failnext 3'))   # armed before anything can request an install
    print(S('rescan').split('\n')[0]); print(S('sigs'))
    check('time_mgr_global resolves to the qword', 'time_mgr_global RESOLVED' in S('sigs'))

    # ---- retry: transient MinHook failures are retried in the background (8 tries, 400 ms apart) and never block the caller
    t0 = time.time()
    S('telemetry off'); S('telemetry on')   # requests skills / event_dispatcher / amp installs
    check('the bridge call returned without waiting for the installs', time.time() - t0 < 0.5, '%.2fs' % (time.time() - t0))
    time.sleep(3.0)
    h = S('hooks')
    extra = sum(int(x) - 1 for x in re.findall(r'hook-install \w+: ok \(after (\d+) attempts?\)', h))
    check('exactly the 3 injected failures were retried (the tasks share the 400 ms rounds)', extra == 3, 'retries=%d' % extra)
    check('all telemetry hooks are installed', all(('hook-install %s: ok' % n) in h for n in ('skills', 'event_dispatcher', 'amp')), h.replace('\n', ' | ')[:500])
    log = open(os.path.join(H, 'CombatMod.log')).read()
    check('each attempt is logged', 'attempt 1/8 failed' in log and 'retrying in 400 ms' in log and re.search(r'hook \w+: installed \(attempt [234]/8\)', log) is not None)
    # a persistent failure: the XInput hooks (requested whenever the panel opens) fail 20 times -> gives up after exactly 8 attempts
    S('hooktest failnext 20'); S('ui show'); time.sleep(3.8)
    h = S('hooks')
    check('gives up after 8 attempts and reports it', 'hook-install xinput: FAILED after 8 attempts' in h, h.replace('\n', ' | ')[:500])
    S('hooktest failnext 0'); S('ui hide'); time.sleep(0.3); S('ui show'); time.sleep(0.8); S('ui hide'); time.sleep(0.3)
    check('a later request starts a fresh cycle that succeeds', 'hook-install xinput: ok' in S('hooks'))

    # ---- panel basics, pad swallow, pause
    check('game sees the pad while closed', (pad(0x1000), time.sleep(0.2), seen())[2] == (0x1000, True))
    pad(0)
    rate0 = sim_rate()
    check('game clock runs at ~1x before the panel opens', 0.8 < rate0 < 1.25, '%.3f' % rate0)
    print(S('ui show')); time.sleep(0.5)
    st = status(); print(st)
    rate1 = sim_rate(0.8)
    check('game clock is frozen while the panel is open', rate1 < 0.05, '%.4f' % rate1)
    ps = S('ui pause status'); print(ps)
    check('pause status shows the hold and our signature', 'held=1' in ps and 'our pause signature' in ps)
    t_f4 = [rd_float(int(g['tobj'], 16) + 16) for _ in range(10)]
    check('the engine progress is kept at ~0 by the keeper', max(t_f4) < 0.06, str(max(t_f4)))
    pad(0x1000, 20000); time.sleep(0.25)
    check('open panel: the game sees an idle pad', seen() == (0, True))
    pad(0)
    S('cfg set UI.PauseGame 0'); time.sleep(0.4)
    rate2 = sim_rate()
    check('unticking "Pause game while open" resumes the game live', 0.8 < rate2 < 1.25, '%.3f' % rate2)
    S('cfg set UI.PauseGame 1'); time.sleep(0.4)
    check('ticking it again freezes it again', sim_rate() < 0.05)
    print(S('ui hide')); time.sleep(0.4)
    rate3 = sim_rate()
    check('closing the panel resumes the game at once', 0.8 < rate3 < 1.25, '%.3f' % rate3)
    f0 = rd_float(int(g['tobj'], 16))
    check('the time controller is back to idle (-1)', f0 == -1.0, str(f0))

    # ---- watchdog: our signature left behind without a request is cleared after ~1 s
    tobj = int(g['tobj'], 16)
    S('writef %x 3600' % (tobj + 8)); S('writef %x 0.001' % tobj)   # A first: the stand-in engine resets an object whose A+B is 0
    time.sleep(0.25)
    left = rd_float(tobj)
    time.sleep(1.6)
    after = rd_float(tobj)
    check('our signature was written (stale pause)', abs(left - 0.001) < 1e-6, str(left))
    check('watchdog cleared it after ~1 s', after == -1.0, str(after))
    print(S('ui pause status').split('\n')[1])

    # ---- the buttons: close without saving / save & close / Esc / help
    r, btn = rows()
    print(r, btn)
    S('cfg preset vanilla'); S('cfg save')
    S('ui show'); time.sleep(0.4)
    S('ui click %d 0.5' % r['SlowMo.ScaleMul']); S('ui click %d 0' % btn[0]); time.sleep(0.3)   # preset Vanilla (no change) then move a slider
    S('ui click %d 1.0' % r['Launch.Mult']); time.sleep(0.2)
    changed = cfg('Launch.Mult')
    check('slider click changed the setting', changed == '20', changed)
    S('ui click %d 1' % btn[1]); time.sleep(0.4)   # "Close without saving"
    check('Close without saving: panel closed and every change undone', kv(status(), 'open') == '0' and cfg('Launch.Mult') == '1' and cfg('SlowMo.ScaleMul') == '1', cfg('Launch.Mult'))
    check('... and the game runs again', 0.8 < sim_rate() < 1.25)
    S('ui show'); time.sleep(0.4)
    S('ui click %d 1.0' % r['HitStop.DurMul']); time.sleep(0.2)
    S('ui click %d 0' % btn[1]); time.sleep(0.4)   # Save & Close
    inifile = open(ini).read()
    check('Save & Close: closed, settings kept and written to the ini', kv(status(), 'open') == '0' and cfg('HitStop.DurMul') == '10' and 'HitStop.DurMul=10' in inifile)
    check('no AutoSave key / removed keys in the saved ini', 'SlowMo.PlayerOnly' not in inifile and 'Launch.MaxValue' not in inifile and 'SlowMo.MinScale' not in inifile and '[Advanced]' in inifile)
    S('cfg preset vanilla')
    S('ui show'); time.sleep(0.3)
    S('cfg set Launch.Mult 7'); S('ui key 27'); time.sleep(0.4)   # Esc
    check('Esc = close without saving', kv(status(), 'open') == '0' and cfg('Launch.Mult') == '1', cfg('Launch.Mult'))
    S('ui show'); time.sleep(0.3)
    S('cfg set Launch.Mult 7'); tap(0x2000); time.sleep(0.4)  # controller B
    check('pad B = close without saving', kv(status(), 'open') == '0' and cfg('Launch.Mult') == '1', cfg('Launch.Mult'))
    S('ui show'); time.sleep(0.5)
    S('cfg set SlowMo.ScaleMul 0.35'); tap(0x0010); time.sleep(0.4)   # Start
    check('pad Start = Save & Close', kv(status(), 'open') == '0' and cfg('SlowMo.ScaleMul') == '0.35' and 'SlowMo.ScaleMul=0.35' in open(ini).read())
    S('cfg preset vanilla'); S('cfg save')

    # ---- help window (replaces the old tooltips / accordion): a separate no-activate tool window docked to the right of the panel
    S('ui show'); time.sleep(0.5)
    import ctypes
    from ctypes import wintypes
    u = ctypes.windll.user32
    found = {'tooltips_class32': 0, 'CombatModHelp': 0, 'CombatModPanel': 0}
    EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, lp):
        buf = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(hwnd, buf, 64)
        pid = wintypes.DWORD()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if buf.value in found and pid.value == p.pid:
            found[buf.value] += 1
        return True
    u.EnumWindows(EnumProc(cb), 0)
    check('no native tooltip windows any more; one panel window and one help window', found['tooltips_class32'] == 0 and found['CombatModHelp'] == 1 and found['CombatModPanel'] == 1, str(found))
    hp = S('ui help'); print(hp)
    check('help window: visible, docked flush to the right edge of the panel, same height, owned by the panel, never activates',
          'help_visible=1' in hp and 'side=right' in hp and 'flush=1' in hp and 'same_height=1' in hp and 'owned=1' in hp and 'noactivate=1' in hp)
    sel = int(kv(status(), 'selected_row'))
    check('help pane shows the selected row', 'subject_row=%d ' % sel in hp and 'hover_row=-1' in hp, hp.split(chr(10))[0])
    r, btn = rows()
    S('ui hover %d' % r['SlowMo.ScaleMul'])
    hp = S('ui help')
    check('hovering a row switches the help pane to that control (name, range, default, current, tip)', 'title=Slow motion: Scale' in hp and 'Range=0.05x to 2.00x' in hp and 'Default=1.00x' in hp and 'tip: Around 0.3' in hp, hp.replace('\n', ' | ')[:300])
    S('ui hover -1')
    check('pointer leaves: back to the selected row', 'subject_row=%d ' % sel in S('ui help'))
    S('ui click %d 1.0' % r['HitStop.DurMul']); time.sleep(0.2)
    S('ui hover %d' % r['HitStop.DurMul'])
    check('the help pane shows the CURRENT value', 'Current=10.0x' in S('ui help'))
    S('ui hover %d' % r['SlowMo.ScaleMul']); S('ui key 40'); time.sleep(0.2)   # keyboard Down: the help follows the selection again
    hp = S('ui help')
    check('keyboard / pad navigation returns the help pane to the selection', 'hover_row=-1' in hp)
    # ---- Blinding Flash section: the panel shows only the rows of the chosen mode and its window follows (the help window with it)
    def prect():
        m_ = re.search(r'panel_rect=(-?\d+),(-?\d+),(-?\d+),(-?\d+) help_rect=(-?\d+),(-?\d+),(-?\d+),(-?\d+)', S('ui help'))
        v_ = [int(x) for x in m_.groups()]
        return v_[3] - v_[1], v_[7] - v_[5]
    S('cfg set BlindingFlash.Mode 0'); time.sleep(0.9)
    h0, hh0 = prect()
    S('cfg set BlindingFlash.Mode 1'); time.sleep(0.9)
    h1, hh1 = prect()
    S('cfg set BlindingFlash.Mode 2'); time.sleep(0.9)
    h2, hh2 = prect()
    S('cfg set BlindingFlash.Mode 0'); time.sleep(0.9)
    h3, hh3 = prect()
    check('the panel grows with the Blinding Flash mode (off < fast < smooth) and shrinks back; the help window keeps the same height', h0 < h1 < h2 and h3 == h0 and hh0 == h0 and hh1 == h1 and hh2 == h2 and hh3 == h3, '%d %d %d %d' % (h0, h1, h2, h3))
    check('bf status / bf log are available', 'BlindingFlash.Mode=0' in S('bf status') and 'no Blinding Flash chains yet' in S('bf log'))
    check('Blinding Flash settings: mode is a 0..2 switch, FinaleKeys is a live key list', S('cfg set BlindingFlash.Mode 3').startswith('ERR') and 'BlindingFlash.FinaleKeys = 1023' in S('cfg get BlindingFlash.FinaleKeys') and 'BlindingFlash.FinaleKeys = 1023,1024' in S('cfg set BlindingFlash.FinaleKeys 1023,1024'))
    S('cfg set BlindingFlash.FinaleKeys 1023')
    png = os.path.join(H, 'panel.bmp')
    S('ui click %d 0.5' % r['SlowMo.ScaleMul'])
    print(S('ui snapshot ' + png)); bmp_to_png(png, os.path.join(H, 'panel_scale.png'))
    S('cfg set HitStop.DurMul 1')
    S('ui hide'); time.sleep(0.3)
    check('closing the panel hides the help window too', 'help_visible=0' in S('ui help'))

    # ---- hit-stop: native blip (0.99, A=B=0.0333) is deepened and lengthened by the heavy-hits preset, slow motion untouched
    S('cfg preset vanilla'); S('cfg preset heavy'); time.sleep(1.2)
    d2 = int(g['d2'], 16)
    vals, mod = [], []
    t_end = time.time() + 4.0
    while time.time() < t_end and not mod:   # the stand-in's workers share one D and the player is learned from them: poll instead of sampling once
        vals += [(rd_float(d2 + LA['kEvScale']), rd_float(d2 + LA['kEvA']), rd_float(d2 + LA['kEvB'])) for _ in range(25)]
        mod = [v for v in vals if abs(v[0] - 0.99 * 0.15) < 1e-3 and abs(v[1] - 0.0333 * 5) < 1e-3]
    check('heavy hits: the blip is deepened (x0.15) and lengthened (x5) when the player caused it', len(mod) >= 1, str(vals[:3]))
    hs = S('hitstop status'); print(hs)
    check('hitstop status counts events', re.search(r'seen=(\d+) modified=(\d+)', hs) and int(re.search(r'seen=(\d+)', hs).group(1)) > 5 and int(re.search(r'modified=(\d+)', hs).group(1)) >= 1)
    check('heavy preset left slow motion and launch alone', cfg('SlowMo.Enabled') == '0' and cfg('Launch.Enabled') == '0' and cfg('HitStop.ScaleMul') == '0.15' and cfg('HitStop.DurMul') == '5')
    slow = [rd_float(int(g['D'], 16) + LA['kEvScale']) for _ in range(10)]
    check('slow-motion event (0.6) untouched while only hit-stop is on', all(abs(x - 0.6) < 1e-4 for x in slow), str(slow[:3]))
    S('cfg preset vanilla'); time.sleep(0.5)
    vals = [rd_float(d2 + LA['kEvScale']) for _ in range(10)]
    check('vanilla restores the blip', all(abs(x - 0.99) < 1e-4 for x in vals), str(vals[:3]))

    # ---- custom slow-motion triggers: start OUR OWN curve through the stand-in time controller when the player's apply-skill call matches a rule
    S('cfg preset vanilla'); S('trigger clear'); time.sleep(0.3)
    print(S('trigger list').split('\n')[0])
    tl = S('trigger list')
    check('trigger commands are registered; after clear only the built-in TowerArrow rule (#17) is left', '#17 TowerArrow' in tl and 'BUILT-IN' in tl and '#1 ' not in tl, tl.split(chr(10))[0])
    check('the built-in rule is the live-tested one (key 1091:5, scale 0.1, in 0.03, hold 0.35, out 1.4, cooldown 3000, firstOnly)', 'key=1091:5|owner=player|scale=0.1|in=0.03|hold=0.35|out=1.4|cooldownMs=3000|firstOnly=1' in tl, tl)
    check('trigger del / disable of the built-in rule is refused with a hint', S('trigger del 17').startswith('ERR') and S('trigger disable 17').startswith('ERR') and 'Triggers.TowerBlast' in S('trigger disable 17'))
    check('invalid rules are refused', S('trigger add key=0').startswith('ERR') and S('trigger add key=5|owner=any').startswith('ERR') and S('trigger add key=5|scale=9').startswith('ERR'))
    add = S('trigger add key=10258|name=E2E|scale=0.3|in=0.2|hold=0.6|out=0.4|cooldownMs=300|firstOnly=0')
    check('trigger add', 'added as #1' in add, add)
    S('cfg set Triggers.Enabled 1'); time.sleep(0.4)
    st0 = S('trigger status')
    seen_scale, seen_A = set(), set()
    t_end = time.time() + 5.0
    while time.time() < t_end and not (0.3 in seen_scale):
        v = rd_float(tobj); a_ = rd_float(tobj + 8)
        if v > 0:
            seen_scale.add(round(v, 3)); seen_A.add(round(a_, 3))
        time.sleep(0.01)
    lg = S('trigger log 5'); print(lg.split('\n')[0])
    check('the stand-in time controller was driven by the trigger: (0.3, 1, A = in+hold = 0.8, B = out = 0.4)', 0.3 in seen_scale and 0.8 in seen_A, '%s %s' % (seen_scale, seen_A))
    check('the firing is logged with key, owner a1, parameters and age', re.search(r'#1 E2E key=10258 a1=0x[0-9A-F]+ scale=0.3 A=0.8 B=0.4 age=[0-9.]+ms\s+FIRED', lg) is not None, lg.split('\n')[0])
    time.sleep(2.5)
    stt = S('trigger status'); print(stt)
    check('while our own curve runs, further matches are skipped as busy (and the curve is NOT restarted)', int(re.search(r'busy=(\d+)', stt).group(1)) >= 2 and int(re.search(r' fired=(\d+)', stt).group(1)) >= 2, stt.split('\n')[1])
    # pause active: never fire
    b0 = int(re.search(r'pause=(\d+)', S('trigger status')).group(1))
    S('ui pause on'); time.sleep(1.8)
    check('pause active: triggers are skipped, nothing is fired over the pause', int(re.search(r'pause=(\d+)', S('trigger status')).group(1)) > b0 and abs(rd_float(tobj) - 0.001) < 1e-6, str(rd_float(tobj)))
    S('ui pause off'); time.sleep(0.5)
    check('after the pause the controller is idle or runs a trigger curve (never stuck at the pause signature)', abs(rd_float(tobj) - 0.001) > 1e-6)
    # a disabled engine: no more firings
    S('cfg set Triggers.Enabled 0'); time.sleep(1.5)
    f1 = int(re.search(r' fired=(\d+)', S('trigger status')).group(1)); time.sleep(1.5)
    check('Triggers.Enabled=0 stops all firing', int(re.search(r' fired=(\d+)', S('trigger status')).group(1)) == f1)
    # a disabled rule never fires; enabling / disabling single rules works
    S('trigger clear')
    S('trigger add name=Off|enabled=0|key=10258|scale=0.15|in=0.1|hold=0.5|out=0.45')
    S('cfg set Triggers.Enabled 1'); f2 = int(re.search(r' fired=(\d+)', S('trigger status')).group(1)); time.sleep(1.5)
    check('a disabled rule (enabled=0) never fires', int(re.search(r' fired=(\d+)', S('trigger status')).group(1)) == f2)
    S('trigger enable 1'); time.sleep(2.0)
    check('... and fires once it is enabled', int(re.search(r' fired=(\d+)', S('trigger status')).group(1)) > f2)
    S('trigger disable 1'); time.sleep(1.8)
    f3 = int(re.search(r' fired=(\d+)', S('trigger status')).group(1)); time.sleep(1.5)
    check('disabling the rule stops it', int(re.search(r' fired=(\d+)', S('trigger status')).group(1)) == f3)
    S('cfg set Triggers.Enabled 0')
    # manual fire for experiments
    time.sleep(1.5)
    mf = S('trigger fire 0.4 0.5 0.5')
    check('trigger fire <scale> <A> <B> writes a curve into the controller', mf.startswith('FIRED') and abs(rd_float(tobj) - 0.4) < 1e-4 and abs(rd_float(tobj + 8) - 0.5) < 1e-4, mf.split('\n')[0])
    check('... and a second fire while it runs is refused without `force`', S('trigger fire 0.4 0.5 0.5').startswith('ERR') and S('trigger fire 0.2 0.5 0.5 force').startswith('FIRED'))
    time.sleep(1.5)
    check('the engine ended the curve by itself (idle again)', rd_float(tobj) == -1.0, str(rd_float(tobj)))
    sv = S('trigger save')
    ini_txt = open(ini).read()
    check('trigger save writes a [Triggers] section', 'saved 1 rule' in sv and '[Triggers]' in ini_txt and 'Trigger1 = ' in ini_txt and 'name=Off' in ini_txt, sv.split('\n')[0])
    S('trigger clear'); S('cfg preset vanilla'); S('cfg save')

    # ---- launch scan (byte-wise, whole process), Launch.MinValue (raise before the multiplier) and Launch.DenyKeys (key:slot)
    S('cfg preset vanilla'); S('launch clear')
    sc = S('launch scan'); print(sc.split(chr(10))[0])
    allscan = S('launch scan all'); print(allscan)
    mrec = re.search(r'(\d+) launch records \(S unique\)', sc)
    check('launch scan finds the stand-in skill records byte-wise (3 workers) and finishes', 'scan complete' in sc and mrec is not None and int(mrec.group(1)) >= 3 and re.search(r'keys=\[1025[38],[123],0,0\]', allscan) is not None, sc.split(chr(10))[0])
    check('launch scan zero is accepted (no 0/0 records in the stand-in)', S('launch scan zero').startswith('scan complete') or 'scan complete' in S('launch scan zero'))
    check('launch scan show works without rescanning', 'launch records' in S('launch scan show'))
    tdir = os.path.join(H, 'CombatMod_telemetry')
    scans = sorted(f for f in os.listdir(tdir) if f.startswith('launch_scan_') and f.endswith('.json'))
    check('launch_scan_<time>.json written to the telemetry directory', len(scans) >= 1, str(scans[-1:]))
    if scans:
        js = json.load(open(os.path.join(tdir, scans[-1]), encoding='utf-8'))
        check('... with the full table (records with keys, launch, angles, zero flag, address)', js['complete'] and js['count'] >= 3 and all({'S', 'keys', 'launch', 'angles', 'zero'} <= set(x) for x in js['records']) and any(x['launch'] == [15, 15] for x in js['records']), str(js['count']))
    td = S('telemetry dump'); m2 = re.search(r'to (.+\.json)', td)
    if m2:
        data2 = json.load(open(m2.group(1).strip(), encoding='utf-8'))
        check('the telemetry session JSON carries the last launch scan', data2.get('launch_scan') and data2['launch_scan']['count'] >= 3, str(type(data2.get('launch_scan'))))
    S('cfg set Launch.Enabled 1'); S('cfg set Launch.Mult 2'); time.sleep(0.8)
    ll = S('launch log 30')
    check('launch boost x2 on the 15/15 stand-in records (30)', 'now=(30,30)' in ll, ll.split(chr(10))[1] if len(ll.split(chr(10))) > 1 else ll)
    S('cfg set Launch.MinValue 50'); time.sleep(0.8)
    ll = S('launch log 30')
    check('Launch.MinValue 50: raised BEFORE the multiplier (50 x 2 = 100), marked RAISED', 'now=(100,100)' in ll and 'RAISED(Launch.MinValue)' in ll and 'raised_now=' in S('launch status'), ll.split(chr(10))[1] if len(ll.split(chr(10))) > 1 else ll)
    S('cfg set Launch.DenyKeys 10253:1,10258:1'); time.sleep(0.8)
    ll = S('launch log 30'); ls = S('launch status')
    check('Launch.DenyKeys key:slot: the records with that first/second key are denied and restored to 15', 'DENIED(Launch.DenyKeys)' in ll and re.search(r'keys=\[1025[38],1,0,0\] orig=\(15,15\) now=\(15,15\) .*DENIED', ll) is not None and 'denied=' in ls, ll.split(chr(10))[1] if len(ll.split(chr(10))) > 1 else ll)
    check('Launch.DenyKeys rejects bad entries and keeps the old list', S('cfg set Launch.DenyKeys 1091:').startswith('ERR') and '10253:1,10258:1' in S('cfg get Launch.DenyKeys'))
    S('cfg set Launch.Enabled 0'); time.sleep(0.5)
    check('switching the boost off restores every record (15)', 'now=(100,100)' not in S('launch log 30') and 'now=(30,30)' not in S('launch log 30'))
    S('cfg preset vanilla'); S('launch clear')

    # ---- `layout check`: the stand-in's event blocks, controller objects and skill records are built from src/gamelayout.h, so everything must match
    S('cfg preset vanilla'); S('cfg set Triggers.Enabled 0'); time.sleep(1.0)
    lc = S('layout check'); print(lc)
    check('layout check: event blocks (type id %d, floats), time-controller objects and skill records all match the constants' % LA['kTimeEventType'], 'RESULT: every checked part of the layout matches' in lc and 'event layout OK' in lc and 'time-controller layout OK' in lc and 'skill-record layout OK' in lc, lc.split(chr(10))[0])
    ls_ = S('layout show')
    check('layout show prints the constants', ('type u16 at +0x%x == %d' % (LA['kEvType'], LA['kTimeEventType'])) in ls_ and ('objects at P+0x%X' % LA['kCtlObj'][0]) in ls_, ls_.split(chr(10))[1])
    kk = S('skills log 3')
    check('the skill capture reads the keys at the layout offset (stand-in keys 10253 / 10258 and the worker id)', re.search(r'key=\[1025[38],[123],0,0\]', kk) is not None, kk.split(chr(10))[1] if len(kk.split(chr(10))) > 1 else kk)

    # ---- B1: the write guard. A patch that moves kCtlObj leaves time_mgr_global matching: the slot then points at an UNRELATED object. Nothing may be written into it.
    rg = S('resolve time_mgr_global')
    G_ = int(re.search(r'0x([0-9A-F]+)', rg).group(1), 16)
    M_ = int(S('readq %x' % G_).split('=')[1].split()[0], 16)
    P_ = int(S('readq %x' % M_).split('=')[1].split()[0], 16)
    slot_ = P_ + LA['kCtlObj'][0]
    orig_ptr = int(S('readq %x' % slot_).split('=')[1].split()[0], 16)
    decoy_ = int(g['d2'], 16)
    before_ = S('read %x 24' % decoy_)
    S('writeq %x %d' % (slot_, decoy_))
    rp = S('ui pause on')
    time.sleep(0.4)
    check('B1: pause REFUSES when a controller slot points at an unrelated object ("cannot pause"), and nothing is written there', rp.startswith('ERR cannot pause') and 'does not look like a time controller' in rp and S('read %x 24' % decoy_) == before_, rp.split(chr(10))[0][:200])
    check('B1: ui status / ui pause status report the refusal', 'pause_write_guard=REFUSING WRITES' in status() and 'write_guard=REFUSING WRITES' in S('ui pause status'))
    S('ui show'); time.sleep(0.6)
    check('B1: opening the panel does not pause the game or write into the unrelated object while the guard refuses', 'held=0' in S('ui pause status') and S('read %x 24' % decoy_) == before_)
    ft = S('trigger fire 0.4 0.5 0.5 force')
    check('B1: the trigger engine refuses as well (controller unreadable), unrelated memory untouched', ft.startswith('ERR') and S('read %x 24' % decoy_) == before_, ft.split(chr(10))[0][:160])
    lc2 = S('layout check')
    check('B1: layout check says the pause and the triggers refuse to write', 'REFUSE to write' in lc2 and 'time-controller layout WRONG' in lc2)
    S('ui hide'); time.sleep(0.3)
    S('writeq %x %d' % (slot_, orig_ptr))   # the slot is right again
    time.sleep(0.2)
    S('ui pause on'); time.sleep(0.4)
    check('B1: with the right object back the guard passes and the pause works again', 'held=1' in S('ui pause status') and 'write_guard=ok' in S('ui pause status'))
    S('ui pause off'); time.sleep(0.4)
    check('B1: ... and resumes (controller idle)', rd_float(tobj) == -1.0, str(rd_float(tobj)))

    # ---- telemetry
    S('cfg preset heavy'); time.sleep(1.0)
    dump = S('telemetry dump'); print(dump)
    m = re.search(r'to (.+\.json)', dump)
    check('telemetry dump wrote a session file', m is not None)
    if m:
        path = m.group(1).strip()
        data = json.load(open(path, encoding='utf-8'))
        name = os.path.basename(path)
        check('file name = <TimeDateStamp>_<date>_<time>.json', re.match(r'^[0-9A-F]{8}_\d{8}_\d{6}\.json$', name) is not None and name[:8] == data['module']['time_date_stamp'][2:].upper().zfill(8), name)
        check('module identity', data['module']['size_of_image'] > 0 and data['module']['file_size'] > 0 and data['module']['exe'].endswith('CrimsonDesert.exe'))
        sigs = {s_['name']: s_ for s_ in data['signatures']}
        check('signature table with RVAs and fingerprints', sigs['time_curve_start']['status'] == 'RESOLVED' and sigs['time_curve_start']['rva'] and len(sigs['time_curve_start']['fingerprint']) == 48 and sigs['time_mgr_global']['status'] == 'RESOLVED')
        hooks = {h_['name']: h_ for h_ in data['hooks']}
        check('hook addresses + pre-hook fingerprints', set(hooks) >= {'time_curve_start', 'apply_attack_skill_to_position', 'event_dispatcher'} and all(len(h_['fingerprint']) == 48 for h_ in hooks.values()))
        ev = {e['type']: e for e in data['events']}
        slots = {s_['type']: s_ for s_ in data['event_table']['slots']}
        check('event catalog: types 3 and 7 with handler RVA, fingerprint, sample D and floats', {3, 7} <= set(ev) and ev[3]['count'] > 5 and len(ev[3]['fingerprint']) == 48 and len(ev[3]['sample_d']) == 192 and len(ev[3]['sample_floats']) == 6, str(sorted(ev)))
        check('dispatcher table enumeration lists the registered types (also those that never fired)', {3, 7} <= set(slots) and slots[3]['fn_rva'] == ev[3]['fn_rva'], str(sorted(slots)))
        keys = {k['key']: k for k in data['skill_keys']}
        check('skill keys with counts and the player flag', {10253, 10258} <= set(keys) and keys[10253]['count'] > 5 and keys[10253]['player_count'] >= 1, str(sorted(keys)))
        check('launch records with the raw S probe', len(data['launch_records']) >= 2 and all(len(r_['s_probe']) == 896 and r_['marker'] == ['0x7F7FFFFF'] * 3 and r_['orig_strength'] == [15, 15] for r_ in data['launch_records']))
        layouts = {(round(x['scale'], 4), round(x['a'], 4), round(x['b'], 4)): x for x in data['slowmo_layouts']}
        check('slow-mo layouts: the 0.6 event and the 0.99 blip with context keys', (0.6, 0.5, 0.2) in layouts and (0.99, 0.0333, 0.0333) in layouts and layouts[(0.99, 0.0333, 0.0333)]['count'] > 5 and layouts[(0.6, 0.5, 0.2)]['keys'], str(list(layouts)[:3]))
        check('hit-stop events per minute with the triggering keys', data['hitstop_per_minute'] and data['hitstop_per_minute'][-1]['count'] > 5 and data['hitstop_per_minute'][-1]['keys'], str(data['hitstop_per_minute'][-1:]))
        import telemetry_diff as td
        f = td.diff(data, json.loads(json.dumps(data)))
        check('telemetry_diff: a file against itself has no changes', not [t for s_, t in f if s_ != 'info'])
        mutated = json.loads(json.dumps(data))
        mutated['signatures'][0]['status'] = 'MISSING'; mutated['signatures'][0]['rva'] = None
        mutated['events'][0]['fingerprint'] = '00' * 24
        f = td.diff(data, mutated)
        check('telemetry_diff: a lost signature and a changed handler are reported as critical', len([1 for s_, _ in f if s_ == 'CRITICAL']) >= 1, td.report(f).split('\n')[0])
        print(S('telemetry status'))
    # ---- shutdown restores everything
    S('ui show'); time.sleep(0.4)
    check('frozen again before quit-bridge', sim_rate(0.5) < 0.05)
    print(S('quit-bridge'))
    time.sleep(0.8)
    log = open(os.path.join(H, 'CombatMod.log')).read()
    print('--- log (pause/ui/hook):'); print('\n'.join(l for l in log.split('\n') if re.search(r'pause:|ui: panel|hook (amp|skills|event|xinput)|telemetry:', l))[-3000:])
    check('the pause log has timestamped PAUSED / RESUMED / WATCHDOG entries', 'pause: PAUSED' in log and 'pause: RESUMED' in log and 'pause: WATCHDOG' in log)
finally:
    p.kill(); p.wait()
    try: os.remove(PAD)
    except OSError: pass
    txt = open(ini).read()
    open(ini, 'w').write(re.sub(r'\n\[Cinematic\].*', '', txt, flags=re.S))
bad = [n for n, ok in RESULTS if not ok]
print('\nE2E: %d checks, %d failed%s' % (len(RESULTS), len(bad), (': ' + ', '.join(bad)) if bad else ''))
sys.exit(1 if bad else 0)

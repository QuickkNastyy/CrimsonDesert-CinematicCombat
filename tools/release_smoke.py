#!/usr/bin/env python3
"""Smoke test of the RELEASE package on the stand-in host (NOT the game). Offline: port 6401 only, never 6400, never the game folder.

    python tools\\release_smoke.py [CombatMod_<version>.zip] [--demo-hazards] [--keep]

The zip (default: the newest one in release\\out) is extracted to build\\release_smoke\\<scenario>\\ together with the stand-in host
(build\\harness\\CrimsonDesert.exe + the fake xinput1_4.dll, built by tools\\harness.bat). Only these edits are made to the shipped ini for the test:
BridgeEnabled=1 + BridgePort=6401 (so the test can talk to the plugin) and, for the panel scenario, [UI] TestMode=1 (window parked off-screen).
All Allow* flags stay 0, exactly as shipped (except in --demo-hazards, which flips AllowWrite for one throw-away run).

Scenarios
  A  shipped files unchanged                : starts, opens NO listening socket (bridge off), no telemetry folder, log is clean
  B  unsupported build (real CombatMod.sigs on the stand-in; none of the 8 signatures exist there): inert, nothing hooked, panel still opens, process alive
  C  CombatMod.sigs missing                 : inert, process alive
  D  corrupt / hostile ini (several variants): process alive, every setting stays in range, log says what was ignored
  E  supported build (stand-in signatures)  : hooks install with all Allow* = 0, slow motion and launch are applied with the shipped defaults,
                                              Launch.DenyKeys active, dev commands refused, panel open/close pauses and RESTORES the clock,
                                              `cfg save` keeps the other sections of the ini, the plugin's own `selftest` passes
  F  (--demo-hazards) the pause chain hazard: a wrong controller offset used to get 20 bytes written into unrelated memory. Since 0.9.0-beta the
                      pause REFUSES (both controller objects are classified before any write); the demo now passes when nothing is written.
  G  the bridge / telemetry gates           : a missing ini, a UTF-16 ini, an ini with BridgeEnabled=1 but WITHOUT the developer marker file
                                              CombatMod.dev: no listening socket, no telemetry folder
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bridge_client import send   # noqa: E402  (refuses port 6400 unless COMBATMOD_LIVE=1)

ROOT = os.path.abspath(os.path.join(HERE, '..'))
HARNESS = os.path.join(ROOT, 'build', 'harness')
BASE = os.path.join(ROOT, 'build', 'release_smoke')
PORT = 6401
results = []


def check(name, cond, detail=''):
    results.append((name, bool(cond)))
    print('%s %s%s' % ('PASS' if cond else 'FAIL', name, ('  - ' + str(detail)) if detail != '' else ''))
    return bool(cond)


def S(cmd, timeout=8.0):
    return send(cmd, port=PORT, timeout=timeout)


def listeners_of(pid):
    out = subprocess.run(['netstat', '-ano', '-p', 'tcp'], capture_output=True, text=True).stdout
    return sorted({l.split()[1] for l in out.splitlines() if 'LISTENING' in l and l.split()[-1] == str(pid)})


def port_owners(port):
    out = subprocess.run(['netstat', '-ano', '-p', 'tcp'], capture_output=True, text=True).stdout
    return {int(l.split()[-1]) for l in out.splitlines() if ':%d ' % port in l and 'LISTENING' in l}


class Host:
    """One stand-in process in its own folder. Killed by PID only (the real game has the same process name!)."""

    def __init__(self, name, zpath, ini=None, sigs=None, ini_bytes=None, marker=False):
        self.dir = os.path.join(BASE, name)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        for f in ('CrimsonDesert.exe', 'xinput1_4.dll'):
            shutil.copy2(os.path.join(HARNESS, f), self.dir)
        with zipfile.ZipFile(zpath) as z:
            z.extractall(self.dir)
        if ini is False:
            os.remove(os.path.join(self.dir, 'CombatMod.ini'))
        elif ini_bytes is not None:
            open(os.path.join(self.dir, 'CombatMod.ini'), 'wb').write(ini_bytes)
        elif ini is not None:
            open(os.path.join(self.dir, 'CombatMod.ini'), 'w', newline='').write(ini)
        if sigs is not None:
            if sigs is False:
                os.remove(os.path.join(self.dir, 'CombatMod.sigs'))
            else:
                open(os.path.join(self.dir, 'CombatMod.sigs'), 'w').write(sigs)
        if marker:   # what a developer adds next to the asi so that BridgeEnabled=1 is honoured; never part of the zip
            open(os.path.join(self.dir, 'CombatMod.dev'), 'w').write('developer marker\n')
        self.p = None
        self.out = None

    def ini_text(self):
        return open(os.path.join(self.dir, 'CombatMod.ini'), encoding='utf-8', errors='replace', newline='').read()

    def log(self):
        p = os.path.join(self.dir, 'CombatMod.log')
        return open(p, encoding='utf-8', errors='replace').read() if os.path.exists(p) else ''

    def start(self, wait_bridge=True):
        if PORT in port_owners(PORT) or port_owners(PORT):
            raise SystemExit('ABORT: port %d is already in use by %s' % (PORT, port_owners(PORT)))
        self.out = open(os.path.join(self.dir, 'out.txt'), 'w')
        self.p = subprocess.Popen([os.path.join(self.dir, 'CrimsonDesert.exe')], cwd=self.dir, stdout=self.out, stderr=subprocess.STDOUT, creationflags=0x08000000)
        if not wait_bridge:
            time.sleep(3.0)
            return
        for _ in range(60):
            time.sleep(0.25)
            if self.p.poll() is not None:
                raise SystemExit('ABORT: the stand-in host exited at start (code %s)' % self.p.returncode)
            if self.p.pid in port_owners(PORT):
                break
        owners = port_owners(PORT)
        if owners != {self.p.pid}:
            self.stop()
            raise SystemExit('ABORT: port %d is owned by %s, not by the stand-in host (pid %d)' % (PORT, sorted(owners), self.p.pid))
        if 6400 in [int(x.split(':')[-1]) for x in listeners_of(self.p.pid)]:
            self.stop()
            raise SystemExit('ABORT: the stand-in host listens on 6400')
        time.sleep(1.0)

    def alive(self):
        return self.p is not None and self.p.poll() is None

    def addrs(self):
        first = open(os.path.join(self.dir, 'out.txt')).read().split('\n')[0]
        return dict(re.findall(r'(\w+)=([0-9a-f]+)', first))

    def stop(self):
        if self.p is not None and self.p.poll() is None:
            self.p.kill()
            self.p.wait()
        if self.out:
            self.out.close()


def bridged(ini_text, extra_ui=False, allow_write=False):
    t = ini_text.replace('BridgeEnabled=0', 'BridgeEnabled=1\r\nBridgePort=%d' % PORT if '\r\n' in ini_text else 'BridgeEnabled=1\nBridgePort=%d' % PORT, 1)
    if allow_write:
        t = t.replace('AllowWrite=0', 'AllowWrite=1')
    if extra_ui:
        t = t.replace('StartOpen=0', 'StartOpen=0\r\nTestMode=1', 1)
    return t


def kv(text, key):
    m = re.search(r'\b' + re.escape(key) + r'=(\S+)', text)
    return m.group(1) if m else None


def cfg_all():
    d = {}
    for line in S('cfg list').split('\n'):
        m = re.match(r'(\S+) = (\S+)\s+\[(.*?)\]', line)
        if m:
            d[m.group(1)] = (m.group(2), m.group(3))
    return d


def in_range(cfg):
    bad = []
    for k, (v, rng) in cfg.items():
        m = re.match(r'([-\d.e+]+)\.\.([-\d.e+]+) step', rng)
        if m:
            try:
                x = float(v)
            except ValueError:
                bad.append(k)
                continue
            if not (float(m.group(1)) - 1e-6 <= x <= float(m.group(2)) + 1e-6):
                bad.append('%s=%s' % (k, v))
    return bad


def sigline(host, addr, n=40):
    rd = S('read %s %d' % (addr, n))
    hb = []
    for line in rd.split('\n')[:4]:
        for tok in line.split()[1:]:
            if len(tok) == 2 and all(c in '0123456789ABCDEF' for c in tok):
                hb.append(tok)
    return hb[:n]


def standin_sigs(host):
    g = host.addrs()
    gb = sigline(host, g['getg'], 24)
    pat = ' '.join(gb[:3] + ['??'] * 4 + gb[7:9] + ['??'] * 4 + gb[13:18])
    return ('time_curve_start = %s | off=0 | deref=0\n' % ' '.join(sigline(host, g['curve'])) +
            'apply_attack_skill_to_position = %s | off=0 | deref=0\n' % ' '.join(sigline(host, g['apply'])) +
            'event_dispatcher = %s | off=0 | deref=0\n' % ' '.join(sigline(host, g['disp'])) +
            'time_mgr_global = %s | off=0 | rip=3 | deref=0\n' % pat), gb


# ---------------------------------------------------------------------------------------------------------------- scenarios
def scen_a(z, shipped):
    h = Host('A_shipped_unchanged', z)
    try:
        h.start(wait_bridge=False)
        time.sleep(4.0)
        check('A: the zip contains no developer marker file', not os.path.exists(os.path.join(h.dir, 'CombatMod.dev')))
        check('A: process alive after 7 s', h.alive())
        check('A: NO listening socket (bridge off as shipped)', not listeners_of(h.p.pid), listeners_of(h.p.pid))
        log = h.log()
        check('A: log: BridgeEnabled=0 line', 'BridgeEnabled=0: no TCP bridge' in log)
        check('A: log: telemetry Enabled=0', 'telemetry: Enabled=0' in log)
        check('A: no CombatMod_telemetry folder created', not os.path.exists(os.path.join(h.dir, 'CombatMod_telemetry')))
        check('A: log: shipped ini loaded without ignored keys', re.search(r'settings: \d+ settings loaded\s*$', log, re.M) is not None and 'ignored' not in log.split('settings:')[1].split('\n')[0], [l for l in log.split('\n') if 'settings:' in l][:2])
        check('A: log: no exception / SEH line', not re.search(r'exception|SEH', log, re.I))
        check('A: log: allow flags all 0', 'AllowWrite=0 AllowTrace=0 AllowWatch=0 AllowInput=0 AllowCall=0' in log)
        check('A: log: UI ready', 'ui: ready' in log)
        first = [l for l in log.split('\n') if l.strip()][:4]
        check('A: log: the first lines name the plugin version and the game build', any('CombatMod 0.9.0-beta' in l and 'game build' in l and 'size_of_image' in l for l in first), first[:3])
        n_sigs = re.search(r'sigs: (\d+)/(\d+) resolved', log)
        check('A: log: stand-in is not the game, 0/8 signatures resolved (inert)', n_sigs and n_sigs.group(1) == '0' and n_sigs.group(2) == '8', n_sigs.group(0) if n_sigs else '')
    finally:
        h.stop()


def scen_b(z, shipped):
    h = Host('B_unsupported_build', z, ini=bridged(shipped, extra_ui=True), marker=True)
    try:
        h.start()
        time.sleep(3.0)
        st = S('status')
        check('B: sigs resolved=0 total=8 (the game build does not match)', 'sigs resolved=0 total=8' in st, st.split('\n')[1] if '\n' in st else st)
        check('B: allow flags 0 as shipped', 'allow write=0 trace=0 watch=0 input=0 call=0' in st)
        hooks = S('hooks')
        check('B: nothing hooked', 'installed' not in hooks.lower() or 'PENDING' not in hooks, hooks.strip().replace('\n', ' | ')[:200])
        log = h.log()
        check('B: log names the failure for the hooks (not retried: signature missing)', 'hook amp: attempt 1 failed, not retrying' in log and 'hook skills: attempt 1 failed, not retrying' in log,
              [l[l.find('hook'):] for l in log.split('\n') if 'hook ' in l][:3])
        ui = S('ui show')
        time.sleep(0.5)
        ps = S('ui pause status')
        check('B: panel opens', 'panel open' in ui, ui.strip().replace('\n', ' '))
        check('B: pause is a no-op without the time_mgr_global signature (no chain => no write)', 'held=0' in ps, ps.split('\n')[0])
        S('ui hide')
        time.sleep(0.5)
        check('B: process alive after 6 s with every feature switched on', h.alive())
        check('B: shipped features are switched on in the settings (they are inert only because nothing could be hooked)', cfg_all().get('SlowMo.Enabled', ('?',))[0] == '1' and cfg_all().get('Launch.Enabled', ('?',))[0] == '1')
    finally:
        h.stop()


def scen_c(z, shipped):
    h = Host('C_sigs_missing', z, ini=bridged(shipped), sigs=False, marker=True)
    try:
        h.start()
        time.sleep(2.5)
        st = S('status')
        check('C: sigs resolved=0 total=0', 'sigs resolved=0 total=0' in st, st.split('\n')[1])
        log = h.log()
        check('C: log: "cannot open sigs file"', 'sigs: cannot open sigs file' in log)
        check('C: hooks refused, not retried', 'hook amp: attempt 1 failed, not retrying' in log)
        check('C: process alive', h.alive())
        r = S('rescan')
        check('C: rescan with no file stays at 0', 'resolved=0' in r)
    finally:
        h.stop()


def settings_in_table():
    n = 0
    with open(os.path.join(ROOT, 'src', 'settings.cpp'), encoding='utf-8') as f:
        for ln in f:
            if re.match(r'\s*\{"[\w.]+",\s*"', ln) and 'CfgType::' in ln:
                n += 1
    return n


def scen_d(z, shipped):
    base = bridged(shipped)
    n_set = settings_in_table()
    head = base[:base.index('[UI]')]            # [General] with the bridge lines
    junk = bytes(range(256)) * 8
    variants = {
        'D1_binary_junk_appended': base.encode() + b'\r\n' + junk + b'\r\n[Cinematic\r\nSlowMo.ScaleMul\r\n=\r\n====\r\n' + b'A' * 200000 + b'\r\n',
        'D2_truncated': base[:int(len(base) * 0.62)].encode(),
        'D3_bad_values': (head + '[Cinematic]\r\nSlowMo.Enabled=maybe\r\nSlowMo.ScaleMul=abc\r\nSlowMo.RampInMul=1e999\r\nSlowMo.RampOutMul=nan\r\nLaunch.Mult=-5\r\n'
                          'Launch.MinValue=999999\r\nLaunch.Enabled=\r\nHitStop.DurMul=0x10\r\nBlindingFlash.Mode=7\r\nUnknown.Key=1\r\n'
                          '[Advanced]\r\nSlowMo.DenyKeys=,,,abc,99999999999,0\r\nLaunch.DenyKeys=1091:x,:5,5:,a:b\r\nBlindingFlash.FinaleKeys=' + ','.join(str(i) for i in range(1, 80)) + '\r\n'
                          '[Triggers]\r\nTrigger1 = key=|||=\r\nTrigger2 = key=1|scale=9|in=-1\r\nTrigger3 = ' + 'x' * 5000 + '\r\n').encode(),
        'D4_utf8_bom_before_first_line': bytes([0xEF, 0xBB, 0xBF]) + base.encode(),
        'D5_empty_file_with_bridge_lines_only': head.encode(),
    }
    for name, data in variants.items():
        h = Host(name, z, ini_bytes=data, marker=True)
        try:
            h.start()
            time.sleep(2.0)
            check('%s: process alive and the bridge answers' % name, h.alive() and S('ping').startswith('pong'))
            cfg = cfg_all()
            check('%s: all %d settings readable, all in range' % (name, n_set), len(cfg) == n_set and not in_range(cfg), in_range(cfg) or len(cfg))
            log = h.log()
            check('%s: log: no exception / SEH' % name, not re.search(r'exception|SEH', log, re.I))
            if name.startswith('D3'):
                check('%s: bad values reported in the log' % name, 'ignored' in log, [l[-160:] for l in log.split('\n') if 'load: ignored' in l][:1])
            if name.startswith('D4'):
                check('%s: a UTF-8 BOM in front of the (comment) first line does not hide the settings' % name, cfg.get('SlowMo.ScaleMul', ('',))[0] == '0.5' and cfg.get('Launch.MinValue', ('',))[0] == '25', cfg.get('SlowMo.ScaleMul'))
        finally:
            h.stop()


def scen_e(z, shipped):
    h = Host('E_supported_standin', z, ini=bridged(shipped, extra_ui=True), sigs='# written by the test after start-up\n', marker=True)
    try:
        h.start()
        time.sleep(1.5)
        sigs, gb = standin_sigs(h)
        check('E: stand-in controller prologue has the expected shape', gb[:3] == ['48', '8B', '05'] and gb[7:9] == ['C7', '05'] and gb[17] == 'C3', ' '.join(gb))
        open(os.path.join(h.dir, 'CombatMod.sigs'), 'w').write(sigs)
        r = S('rescan')
        check('E: 4 stand-in signatures resolve (each exactly once)', 'resolved=4' in r, r.strip())
        S('cfg set SlowMo.Enabled 1')   # nudges the settings handler: the hooks are requested again, now that the signatures exist
        time.sleep(4.0)
        hooks = S('hooks')
        log = h.log()
        check('E: amp hook installed with every Allow* flag at 0', 'hook-install amp: ok' in hooks, [l for l in hooks.split('\n') if 'hook-install' in l])
        check('E: skills hook installed with AllowTrace=0 (needed for player-only and launch)', 'hook-install skills: ok' in hooks, [l for l in hooks.split('\n') if 'hook-install' in l])
        st = S('amp status')
        mod = kv(st, 'modified')
        check('E: amplifier modifies events', mod is not None and int(mod) > 0, 'modified=%s' % mod)
        lg = S('amp log 60')
        check("E: shipped defaults applied to the player's event: (0.6, 0.5, 0.2) -> scale x0.5, ramps x1.6 = (0.3, 0.8, 0.32)",
              'orig(s=0.6 A=0.5 B=0.2) applied(s=0.3 A=0.8 B=0.32)' in lg, [l for l in lg.split('\n') if 'applied(s=0.3 ' in l][:1])
        check("E: a non-player owner's event is left alone (owner_mismatch)", 'applied(s=0.6 A=0.5 B=0.2)' in lg and 'filtered=owner_mismatch' in lg)
        check('E: hit-stop class event (0.99) is untouched while HitStop is off', 'orig(s=0.99 A=0.0333 B=0.0333) applied(s=0.99 A=0.0333 B=0.0333)' in lg)
        lg2 = S('launch log 8')
        check("E: launch: player's record 15 -> 50 (min launch 25, then x2); other owner's record unchanged",
              re.search(r'player=yes .*orig=\(15,15\) now=\(50,50\).*RAISED\(Launch\.MinValue\)', lg2) is not None and re.search(r'player=no .*now=\(15,15\)', lg2) is not None,
              [l[:150] for l in lg2.split('\n') if 'S=' in l])
        cfgv = cfg_all()
        check('E: Launch.DenyKeys active = built-in default 1091:5,60002', cfgv.get('Launch.DenyKeys', ('',))[0] == '1091:5,60002', cfgv.get('Launch.DenyKeys'))
        check('E: SlowMo.DenyKeys active = built-in default', cfgv.get('SlowMo.DenyKeys', ('',))[0] == '10010,10031,10072,10278,10279,10086', cfgv.get('SlowMo.DenyKeys'))
        check('E: telemetry is off', 'enabled=0' in S('telemetry status'))
        check('E: release first-run values: Reaction.Finale=2 (ground throw), Turning Slash reaction 0, Blinding Flash chain speed 1 (locked), unlock off, Minimum launch 25',
              cfgv.get('Reaction.Finale', ('',))[0] == '2' and cfgv.get('Reaction.TurningSlash', ('',))[0] == '0' and cfgv.get('BlindingFlash.ChainSpeed', ('',))[0] == '1' and
              cfgv.get('BlindingFlash.UnlockSpeedUp', ('',))[0] == '0' and cfgv.get('Launch.MinValue', ('',))[0] == '25', {k: cfgv.get(k) for k in ('Reaction.Finale', 'Reaction.TurningSlash', 'BlindingFlash.ChainSpeed', 'BlindingFlash.UnlockSpeedUp', 'Launch.MinValue')})
        e_ = S('cfg set BlindingFlash.ChainSpeed 1.5')
        check('E: Chain speed above 1.0 is refused while locked (it would speed up everyone)', 'UnlockSpeedUp' in e_ and cfg_all().get('BlindingFlash.ChainSpeed', ('',))[0] == '1', e_.strip().split('\n')[0][:150])
        ver_ = S('version')
        check('E: `version` names the plugin version, the layout target and the game exe', 'CombatMod 0.9.0-beta' in ver_ and 'game build 26308950' in ver_ and 'signatures: 4/4 resolved' in ver_, ver_.strip().replace('\n', ' | ')[:200])
        ust = S('ui status')
        check('E: `ui status` shows the version and the pause write guard (ok)', 'version=CombatMod 0.9.0-beta' in ust and 'pause_write_guard=ok' in ust, [l for l in ust.split('\n') if 'guard' in l or 'version' in l][:2])
        lc = S('layout check')
        check('E: `layout check` reports the write guard', 'write guard' in lc, [l for l in lc.split('\n') if 'guard' in l][:1])
        # dev commands reachable with the shipped flags: which are refused?
        refused = {}
        for c in ('write 0 00', 'writeq 0 1', 'patch 0 00', 'trace time_curve_start', 'watch w 0 4', 'key W tap', 'keys W:tap', 'call time_curve_start', 'skills patch add * a2 0 f32 mul 1',
                  'skills on', 'skills raw on', 'trigger add name=x|key=1|scale=0.5', 'trigger fire 0.5 0.1 0.1', 'hooktest failnext 1', 'etrace on', 'amp on'):
            refused[c] = S(c).strip().split('\n')[-1]
        check('E: write / trace / watch / input / call commands answer "ERR disabled"', all(v == 'ERR disabled' for k, v in refused.items() if k.split()[0] in ('write', 'writeq', 'patch', 'trace', 'watch', 'key', 'keys', 'call', 'hooktest')),
              {k: v for k, v in refused.items() if v != 'ERR disabled'})
        print('INFO  E: replies to developer commands with the shipped flags (BridgeEnabled forced to 1):', refused)
        # panel: pause holds the stand-in clock and resume restores it
        tobj = int(h.addrs()['tobj'], 16)
        simt = int(h.addrs()['simtime'], 16)
        import struct

        def rd_double(a):
            v = int(S('readq %x' % a).split('=')[1].split()[0], 16)
            return struct.unpack('<d', struct.pack('<Q', v))[0]

        def rate(sec=0.6):
            a = rd_double(simt); t = time.time(); time.sleep(sec); b = rd_double(simt)
            return (b - a) / (time.time() - t)
        r0 = rate()
        S('ui show')
        time.sleep(0.8)
        r1 = rate()
        ps = S('ui pause status')
        S('ui hide')
        time.sleep(0.8)
        r2 = rate()
        f0 = S('readf %x' % tobj).split('=')[1].strip()
        check('E: panel open pauses the stand-in clock (rate < 0.05) and closing restores it', r1 < 0.05 and r2 > 0.8, 'rate before %.2f open %.3f after %.2f; %s' % (r0, r1, r2, ps.split('\n')[0]))
        check('E: controller object is idle again after the panel closed', f0.startswith('-1'), f0)
        # cfg save keeps the rest of the ini
        before = h.ini_text()
        r = S('cfg save')
        after = h.ini_text()
        check('E: cfg save succeeds', r.rstrip().endswith('OK'), r.strip().replace('\n', ' '))
        keep = ['BridgeEnabled=1', 'BridgePort=%d' % PORT, 'AllowWrite=0', 'AllowTrace=0', 'AllowCall=0', '[UI]', 'ToggleKeyboard=Ctrl+Enter', '[Telemetry]', 'Enabled=0', 'Trigger1 = name=Example|enabled=0']
        check('E: after Save the [General] / [UI] / [Telemetry] / [Triggers] content is intact', all(k in after for k in keep), [k for k in keep if k not in after])
        heads = [l.strip() for l in after.splitlines() if l.strip().startswith('[')]
        check('E: after Save the safety flags are still 0 and the file has one block per section', heads.count('[Cinematic]') == 1 and heads.count('[Advanced]') == 1 and 'AllowWrite=1' not in after and len(heads) == len(set(heads)), heads)
        S('cfg load')
        cfg2 = cfg_all()
        check('E: the saved ini loads back to the same values', all(cfgv[k][0] == cfg2[k][0] for k in cfgv if k in cfg2), [k for k in cfgv if k in cfg2 and cfgv[k][0] != cfg2[k][0]])
        # the plugin's own self-test
        S('cfg preset vanilla')   # the plugin's selftest asserts a default configuration (it reads the live settings), so run it from the defaults
        t = S('selftest', timeout=60.0)
        tail = t.strip().split('\n')
        fails = [l for l in tail if re.search(r'\bFAIL', l)]
        check('E: plugin selftest has no FAIL line', not fails and tail[-1] == 'OK', ('%d lines, last: %s; %s' % (len(tail), tail[-1], fails[:3])))
        check('E: process alive, log has no exception / SEH line', h.alive() and not [l for l in h.log().split('\n') if ' selftest ' not in l and re.search(r'exception|SEH', l, re.I)], [l for l in h.log().split('\n') if ' selftest ' not in l and re.search(r'exception|SEH', l, re.I)][:2])
    finally:
        h.stop()


def scen_f(z, shipped):
    """HAZARD DEMO (B1): the signatures match but the controller slot points at an unrelated block (what a moved kCtlObj[0] looks like). Before 0.9.0-beta the
    pause wrote its signature into that block. Now the pause must REFUSE: the unrelated memory stays byte-identical, the refusal is reported and logged once,
    and the trigger engine refuses too. Needs AllowWrite=1 only for the demo's own raw `writeq` that moves the slot (throw-away run)."""
    h = Host('F_demo_pause_hazard', z, ini=bridged(shipped, extra_ui=True, allow_write=True), sigs='# written by the test after start-up\n', marker=True)
    try:
        h.start()
        time.sleep(1.5)
        sigs, gb = standin_sigs(h)
        open(os.path.join(h.dir, 'CombatMod.sigs'), 'w').write(sigs)
        S('rescan')
        g = h.addrs()
        import layout
        slot_off = layout.L['kCtlObj'][0]
        r = S('resolve time_mgr_global')
        G = int(re.search(r'0x([0-9A-F]+)', r).group(1), 16)
        M = int(S('readq %x' % G).split('=')[1].split()[0], 16)
        P = int(S('readq %x' % M).split('=')[1].split()[0], 16)
        decoy = int(g['d2'], 16)          # an unrelated 0x40-byte block of the host (stand-in for "some other game object")
        before = S('read %x 64' % decoy)
        old_slot = S('readq %x' % (P + slot_off)).split('=')[1].split()[0]
        S('writeq %x %d' % (P + slot_off, decoy))   # simulate a patch that moved kCtlObj[0]: that slot now points at the unrelated block
        pr = S('ui pause on')
        time.sleep(0.6)
        after = S('read %x 64' % decoy)
        ps = S('ui pause status')
        ust = S('ui status')
        lc = S('layout check')
        S('ui pause off')
        check('F: HAZARD NOT REPRODUCED - the unrelated block is byte-identical after `ui pause on`', after == before, 'before=%s after=%s' % (before.split('\n')[0][17:60], after.split('\n')[0][17:60]))
        check('F: the pause answers an error instead of pausing', pr.strip().startswith('ERR') and 'time controller' in pr, pr.strip()[:140])
        check('F: `ui status` / `ui pause status` / `layout check` report the refusal', 'pause_write_guard=REFUSING WRITES' in ust and 'REFUSING WRITES' in ps and 'REFUSE to write' in lc, [l for l in ust.split('\n') if 'guard' in l])
        tf = S('trigger fire 0.5 0.1 0.1')
        after2 = S('read %x 64' % decoy)
        check('F: the custom-trigger engine refuses to write through the same objects (decoy still identical)', after2 == before, tf.strip().split('\n')[0][:120])
        lg = h.log()
        check('F: the refusal is logged once ("WRITE REFUSED")', lg.count('WRITE REFUSED') == 1, lg.count('WRITE REFUSED'))
        S('writeq %x %s' % (P + slot_off, old_slot))   # put the slot back: the guard lets the pause work again
        time.sleep(0.4)
        pr2 = S('ui pause on')
        time.sleep(0.5)
        ps2 = S('ui pause status')
        S('ui pause off')
        check('F: with the correct controller object the same pause works again', not pr2.strip().startswith('ERR') and 'held=1' in ps2, (pr2.strip()[:80], ps2.split('\n')[0]))
    finally:
        h.stop()


def scen_g(z, shipped):
    """B2: a missing / unreadable ini, or BridgeEnabled=1 without the developer marker, must not open port 6400/6401 or create the telemetry folder."""
    cases = [
        ('G1_bridge1_without_marker', dict(ini=bridged(shipped), marker=False), 'BridgeEnabled=1 IGNORED'),
        ('G2_ini_missing', dict(ini=False, marker=True), 'ini: not found, using defaults'),
        ('G3_ini_utf16', dict(ini_bytes=shipped.replace('BridgeEnabled=0', 'BridgeEnabled=1').replace('Enabled=0', 'Enabled=1').encode('utf-16'), marker=True), None),
        ('G4_ini_missing_no_marker', dict(ini=False, marker=False), 'ini: not found, using defaults'),
    ]
    for name, kw, logline in cases:
        h = Host(name, z, **kw)
        try:
            h.start(wait_bridge=False)
            time.sleep(4.0)
            lg = h.log()
            check('%s: process alive' % name, h.alive())
            check('%s: NO listening socket (the bridge stays off)' % name, not listeners_of(h.p.pid), listeners_of(h.p.pid))
            check('%s: no CombatMod_telemetry folder' % name, not os.path.exists(os.path.join(h.dir, 'CombatMod_telemetry')))
            if logline:
                check('%s: log says why (%s)' % (name, logline), logline in lg, [l for l in lg.split('\n') if 'ini' in l or 'Bridge' in l][:2])
            check('%s: log: no exception / SEH' % name, not re.search(r'exception|SEH', lg, re.I))
        finally:
            h.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('zip', nargs='?')
    ap.add_argument('--demo-hazards', action='store_true')
    ap.add_argument('--only', default='ABCDEG', help='scenario letters to run (default ABCDEG)')
    a = ap.parse_args()
    zp = a.zip
    if not zp:
        cands = sorted(glob.glob(os.path.join(ROOT, 'release', 'out', 'CombatMod_*.zip')), key=os.path.getmtime)
        if not cands:
            raise SystemExit('no release zip: run tools\\make_release.py first')
        zp = cands[-1]
    for f in ('CrimsonDesert.exe', 'xinput1_4.dll'):
        if not os.path.exists(os.path.join(HARNESS, f)):
            raise SystemExit('stand-in host missing (%s): run tools\\harness.bat once (it only builds under build\\harness)' % f)
    with zipfile.ZipFile(zp) as z:
        shipped = z.read('CombatMod.ini').decode('ascii')
    print('release_smoke: %s\n' % zp)
    os.makedirs(BASE, exist_ok=True)
    pick = a.only.upper() + ('F' if a.demo_hazards else '')
    for letter, fn in (('A', scen_a), ('B', scen_b), ('C', scen_c), ('D', scen_d), ('E', scen_e), ('G', scen_g), ('F', scen_f)):
        if letter in pick:
            print('--- scenario %s' % letter)
            fn(zp, shipped)
    bad = [n for n, c in results if not c]
    print('\n%d checks, %d FAILED' % (len(results), len(bad)))
    for n in bad:
        print('  FAILED: ' + n)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

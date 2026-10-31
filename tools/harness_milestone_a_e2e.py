"""End-to-end check of the settings / player / launch / cinematic slow-mo against the test host (port 6401). Never touches the real game."""
import collections, os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bridge_client import send

H = os.path.join(HERE, '..', 'build', 'harness')
ini = os.path.join(H, 'CombatMod.ini')
out = open(os.path.join(H, 'out.txt'), 'w')
S = lambda c: send(c, port=6401)


def sigline(name, addr):
    rd = S('read %s 40' % addr)
    hb = []
    for line in rd.split('\n')[:3]:
        for tok in line.split()[1:]:
            if len(tok) == 2 and all(c in '0123456789ABCDEF' for c in tok):
                hb.append(tok)
    return '%s = %s | off=0 | deref=0\n' % (name, ' '.join(hb[:40]))


def start(extra_ini=''):
    txt = open(ini).read()
    txt = re.sub(r'\n\[Cinematic\].*', '', txt, flags=re.S) + extra_ini
    open(ini, 'w').write(txt)
    p = subprocess.Popen([os.path.join(H, 'CrimsonDesert.exe')], cwd=H, stdout=out, stderr=subprocess.STDOUT, creationflags=0x08000000)
    time.sleep(2)
    return p


def stop(p):
    p.kill(); p.wait()


def reasons(n=120):
    c = collections.Counter()
    for line in S('amp log %d' % n).split('\n'):
        if 'ctx=' in line:
            f = re.search(r'filtered=(\w+)', line)
            c[f.group(1) if f else 'amplified'] += 1
    return dict(c)


# harness sigs (the stand-in functions)
p = start()
try:
    g = dict(re.findall(r'(\w+)=([0-9a-f]+)', open(os.path.join(H, 'out.txt')).read().split('\n')[0]))
    with open(os.path.join(H, 'CombatMod.sigs'), 'w') as f:
        f.write(sigline('time_curve_start', g['curve']))
        f.write(sigline('apply_attack_skill_to_position', g['apply']))
    print(S('rescan').split('\n')[0])
    print('--- vanilla start: nothing installed'); print(S('hooks')); print(S('cfg list'))
    print(S('cfg get slowmo.scalemul')); print(S('cfg set SlowMo.ScaleMul 9')); print(S('cfg set SlowMo.Bogus 1')); print(S('cfg set SlowMo.PlayerOnly maybe'))
    print('--- preset cinematic (live)'); print(S('cfg preset cinematic').split('\n')[0])
    time.sleep(1.5)
    print(S('hooks'))
    print(S('player status'))
    print(S('amp status'))
    print('slow-mo reasons (PlayerOnly=1; only the player thread is amplified):', reasons())
    print(S('launch status'))
    print(S('launch log 5'))
    print('--- Launch.PlayerOnly=0 -> every record boosted; Mult=2'); print(S('cfg set Launch.PlayerOnly 0').strip().split('\n')[0]); time.sleep(0.5)
    print(S('launch log 5'))
    print('--- Launch.Mult 3 (derived from the original, not compounding)'); print(S('cfg set Launch.Mult 3').split('\n')[0]); time.sleep(0.5)
    print(S('launch log 2'))
    print('--- Launch.Enabled 0 -> all restored'); print(S('launch off')); time.sleep(0.3)
    print(S('launch log 2'))
    print('--- SlowMo.DenyKeys=10253 (harness alternates 10253/10258)'); print(S('cfg set SlowMo.PlayerOnly 0').split('\n')[0]); print(S('cfg set SlowMo.DenyKeys 10253').split('\n')[0])
    S('amp reset_map'); time.sleep(0.8)
    print(reasons(60))
    print('--- amp set (maps to settings)'); print(S('amp set ScaleMul=0.25 DurMul=2 AllowKeys=none'))
    print(S('cfg get SlowMo.ScaleMul'), S('cfg get SlowMo.RampOutMul'))
    print(S('amp set ScaleMul=9'))
    print('--- save / reload'); print(S('cfg save')); print(open(ini).read()[-900:])
    print(S('cfg preset vanilla').split('\n')[0]); print(S('cfg load').split('\n')[0]); print(S('cfg get SlowMo.ScaleMul'))
    print(S('hooks'))
    print(S('selftest').split('\n')[-14:])
    print(S('quit-bridge'))
    time.sleep(0.3)
finally:
    stop(p)

# startup from the ini: enabled features start by themselves, vanilla ini installs nothing
p = start('\n[Cinematic]\nSlowMo.Enabled=1\nSlowMo.ScaleMul=0.5\nSlowMo.PlayerOnly=0\nSlowMo.DenyKeys=\nLaunch.Enabled=1\nLaunch.Mult=2\nLaunch.PlayerOnly=0\nLaunch.MaxValue=25\n')
try:
    time.sleep(1.5)
    print('--- started with ini:'); print(S('hooks')); print(S('amp status').split('\n')[1]); print(S('launch log 3'))
    print(S('quit-bridge'))
finally:
    stop(p)
    txt = open(ini).read()
    open(ini, 'w').write(re.sub(r'\n\[Cinematic\].*', '', txt, flags=re.S))

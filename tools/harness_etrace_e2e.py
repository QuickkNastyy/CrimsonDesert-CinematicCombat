"""End-to-end check of `etrace` against the test host (port 6401). Never touches the real game."""
import os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_client import send

H = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'harness')
out = open(os.path.join(H, 'out.txt'), 'w')
p = subprocess.Popen([os.path.join(H, 'CrimsonDesert.exe')], cwd=H, stdout=out, stderr=subprocess.STDOUT, creationflags=0x08000000)
S = lambda c: send(c, port=6401)
try:
    time.sleep(2)
    g = dict(re.findall(r'(\w+)=([0-9a-f]+)', open(os.path.join(H, 'out.txt')).read().split('\n')[0]))
    rd = S('read %s 40' % g['disp'])
    hb = []
    for line in rd.split('\n')[:3]:
        for tok in line.split()[1:]:
            if len(tok) == 2 and all(c in '0123456789ABCDEF' for c in tok):
                hb.append(tok)
    with open(os.path.join(H, 'CombatMod.sigs'), 'w') as f:
        f.write('event_dispatcher = %s | off=0 | deref=0\n' % ' '.join(hb[:40]))
    print(S('rescan')); print(S('sigs'))
    print(S('etrace status'))
    print(S('etrace on'))
    time.sleep(1.5)
    print(S('etrace status'))
    print(S('etrace log 3'))
    print(S('etrace summary 5'))
    print(S('etrace filter exclude=7'))
    S('etrace clear'); time.sleep(0.6)
    log = S('etrace log 50')
    types = set(re.findall(r' type=(\w+) ', log))
    print('types logged with exclude=7 (expect only 3):', types)
    print(S('etrace status'))
    print(S('etrace filter exclude=none'))
    S('etrace clear'); time.sleep(0.5)
    tl = S('etrace log 5').split('\n')
    t_mid = int(re.search(r' t=(\d+)us', tl[2]).group(1))
    print(S('etrace window %d 30 30' % t_mid)[:1500])
    print(S('etrace filter exclude=abc'))
    print(S('etrace window 1 2'))
    print(S('hooks'))
    print(S('etrace off'))
    print(S('selftest').split('\n')[-12:])
    print(S('quit-bridge'))
    time.sleep(0.5)
finally:
    p.kill()
    p.wait()

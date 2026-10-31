"""End-to-end check of `skills` + amp context filters against the test host (port 6401). Never touches the real game."""
import re, subprocess, sys, time, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_client import send
import layout
LA = layout.L   # src/gamelayout.h

H = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'harness')
out = open(os.path.join(H, 'out.txt'), 'w')
p = subprocess.Popen([os.path.join(H, 'CrimsonDesert.exe')], cwd=H, stdout=out, stderr=subprocess.STDOUT, creationflags=0x08000000)
S = lambda c: send(c, port=6401)
try:
    time.sleep(2)
    txt = open(os.path.join(H, 'out.txt')).read().split('\n')[0]
    g = dict(re.findall(r'(\w+)=([0-9a-f]+)', txt))

    def sigline(name, addr):
        rd = S('read %s 40' % addr)
        hb = []
        for line in rd.split('\n')[:3]:
            for tok in line.split()[1:]:
                if len(tok) == 2 and all(c in '0123456789ABCDEF' for c in tok):
                    hb.append(tok)
        return '%s = %s | off=0 | deref=0\n' % (name, ' '.join(hb[:40]))

    with open(os.path.join(H, 'CombatMod.sigs'), 'w') as f:
        f.write(sigline('time_curve_start', g['curve']))
        f.write(sigline('apply_attack_skill_to_position', g['apply']))
    print(S('rescan')); print(S('sigs'))
    Dd = int(g['D'], 16)

    def dvals():
        return [S('readf %x' % (Dd + o)).split('\n')[0].split('=')[1].strip() for o in (LA['kEvScale'], LA['kEvA'], LA['kEvB'])]

    print(S('skills status'))
    print(S('skills on'))
    time.sleep(1.0)
    print(S('skills status'))
    print(S('skills log 4'))
    print(S('skills summary 5'))
    print(S('amp set ScaleMul=0.5 DurMul=2'))
    print(S('amp on'))
    time.sleep(1.0)
    print(S('amp log 3'))

    def reasons(n=300):
        c = collections.Counter()
        keys = collections.Counter()
        for line in S('amp log %d' % n).split('\n'):
            m = re.search(r'ctx=\(a1=(0x[0-9A-F]+),keys=(\d+)/', line)
            f = re.search(r'filtered=(\w+)', line)
            if 'ctx=' in line:
                c[(f.group(1) if f else 'amplified', m.group(1) if m else 'none', m.group(2) if m else '-')] += 1
        return c

    print('--- no filters, baseline:'); S('amp reset_map'); time.sleep(0.6); print(dict(reasons(60)))
    print(S('amp set DenyKeys=10253'))
    S('amp reset_map'); time.sleep(0.8)
    print('--- DenyKeys=10253 (expect 10253 filtered=deny_key, 10258 amplified):', dict(reasons(60)))
    print(S('amp set DenyKeys=none AllowKeys=10258 OnlyOwner=0x1000'))
    S('amp reset_map'); time.sleep(0.8)
    print('--- Allow 10258 + owner 0x1000 (expect only a1=0x1000,key 10258 amplified):', dict(reasons(60)))
    print(S('amp set AllowKeys=none OnlyOwner=auto'))
    S('amp reset_map'); time.sleep(0.6)
    print('--- OnlyOwner=auto, not learned (expect all owner_unlearned):', dict(reasons(30)))
    print(S('amp learn_owner'))
    time.sleep(31)
    print(S('amp status'))
    S('amp reset_map'); time.sleep(0.6)
    print('--- OnlyOwner=auto learned:', dict(reasons(60)))
    print(S('amp set OnlyOwner=none RequireCtx=1 ContextWindowMs=50'))
    S('amp reset_map'); time.sleep(0.6)
    print('--- RequireCtx=1 window 50ms (same-thread ctx is ~<20ms old => amplified):', dict(reasons(30)))
    print(S('amp set ScaleMul=9'))
    print(S('amp set AllowKeys=0'))
    print(S('amp set OnlyOwner=zz'))
    print(S('amp set ContextWindowMs=10'))
    print(S('amp off')); time.sleep(0.3)
    print('after off (expect 0.6,0.5,0.2):', dvals())
    print(S('skills summary 60'))
    print(S('skills clear')); print(S('skills status'))
    print(S('skills off'))
    print(S('hooks'))
    print(S('selftest'))
    print(S('quit-bridge'))
    time.sleep(0.5)
finally:
    p.kill()
    p.wait()

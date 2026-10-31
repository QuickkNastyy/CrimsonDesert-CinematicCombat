"""End-to-end check of `skills patch` against the test host (port 6401). Never touches the real game."""
import os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bridge_client import send

H = os.path.join(HERE, '..', 'build', 'harness')
out = open(os.path.join(H, 'out.txt'), 'w')
p = subprocess.Popen([os.path.join(H, 'CrimsonDesert.exe')], cwd=H, stdout=out, stderr=subprocess.STDOUT, creationflags=0x08000000)
S = lambda c: send(c, port=6401)
try:
    time.sleep(2)
    g = dict(re.findall(r'(\w+)=([0-9a-f]+)', open(os.path.join(H, 'out.txt')).read().split('\n')[0]))
    rd = S('read %s 40' % g['apply'])
    hb = []
    for line in rd.split('\n')[:3]:
        for tok in line.split()[1:]:
            if len(tok) == 2 and all(c in '0123456789ABCDEF' for c in tok):
                hb.append(tok)
    open(os.path.join(H, 'CombatMod.sigs'), 'w').write('apply_attack_skill_to_position = %s | off=0 | deref=0\n' % ' '.join(hb[:40]))
    print(S('rescan').split('\n')[0])
    print(S('skills patch list'))
    print(S('skills patch add k0=10253 S 100 u32 set 7777'))        # harness key0 alternates 10253/10258; a1 = 0x1000*id
    print(S('skills patch add any=10258 S 104 u32 add 5 owner=player'))
    print(S('skills patch add * a2 8 f32 mul 2.5 owner=0x1000'))
    time.sleep(1.0)
    print(S('skills patch list'))
    print(S('skills patch log 4'))
    print(S('skills status'))
    # restore proof: harness S+0x100 (key0) is rewritten by the worker each loop; the raw capture must show original keys (10253/10258), never 7777
    print(S('skills raw both 0 512 0 0'))
    time.sleep(0.7)
    raw = S('skills rawget last S 256 4')
    print(raw.split('\n')[:3])
    raw_after = S('skills rawget last S_after 256 4')
    print(raw_after.split('\n')[:3])
    for bad in ['skills patch add k0=10253 S 1000 u32 set 1', 'skills patch add k0=10253 S 10 f32 set inf', 'skills patch add k0=0 S 10 f32 set 1', 'skills patch add * S 10 f64 set 1', 'skills patch del 999', 'skills patch', 'skills patch add * S 10']:
        print(bad, '->', S(bad).split('\n')[0])
    print(S('skills patch off')); print(S('skills patch list').split('\n')[0])
    print(S('skills patch on'))
    print(S('skills patch del 1')); print(S('skills patch clear')); print(S('skills patch list'))
    print(S('selftest').split('\n')[-4:])
    print(S('quit-bridge'))
    time.sleep(0.4)
finally:
    p.kill()
    p.wait()

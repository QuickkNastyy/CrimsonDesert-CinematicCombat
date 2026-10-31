"""End-to-end check of `skills raw*` and `mem scanf` against the test host (port 6401). Never touches the real game."""
import os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bridge_client import send
import layout
LA = layout.L   # src/gamelayout.h
import skillraw

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
    print(S('skills raw on 99999'))
    print(S('skills raw both 64 64'))
    print(S('skills raw both'))
    time.sleep(1.0)
    print(S('skills rawlist 3'))
    print(S('skills rawget last S 0x%x 32' % LA['kSKeys']))
    print(S('skills rawget last a2_after 0 64'))
    print(S('skills rawget 99999 S'))
    print(S('skills rawget last nonsense'))
    print(S('skills raw on'))
    time.sleep(0.5)
    print(S('skills rawget last S_after'))
    print(S('skills raw both 256 256 256 256'))
    time.sleep(0.5)
    for bad in [r'C:\Windows\Temp\x.bin', r'C:\Modding\Crimson-Desert\..\evil.bin', r'C:\Modding\Crimson-Desert\nonexistent_dir\x.bin', r'relative.bin', r'C:\Modding\Crimson-Desert']:
        print('rawsave', bad, '->', S('skills rawsave ' + bad).split('\n')[0])
    path = os.path.abspath(os.path.join(H, 'raw_e2e.bin'))
    print(S('skills rawsave ' + path).split('\n')[0])
    caps = skillraw.load(path)
    print('python loader: %d captures, first: idx=%d key0=%d sizes=%s has_after=%s a1=0x%X' % (len(caps), caps[0]['idx'], caps[0]['key0'], caps[0]['sz'], caps[0]['has_after'], caps[0]['base'][3]))
    print(S('mem scanf %s 64 0 1' % g['D']).split('\n')[:3])
    Dd = g['D']
    print(S('mem scanf %s 32 0.0 1000' % Dd))
    print(S('mem scanf zz 64 0 1'))
    print(S('mem scanf %s 64 5 1' % Dd))
    print(S('skills raw off')); print(S('quit-bridge'))
    time.sleep(0.5)
finally:
    p.kill()
    p.wait()
    try: os.remove(os.path.join(H, 'raw_e2e.bin'))
    except OSError: pass

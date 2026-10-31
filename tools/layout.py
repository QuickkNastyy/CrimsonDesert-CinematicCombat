"""The build-specific game-layout numbers for the Python tools: parsed from src/gamelayout.h, the single source of truth.
    import layout; layout.L['kEvScale']  ->  12        layout.L['kCtlObj']  ->  [0xCD8, 0xCE8]
"""
import os
import re

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'gamelayout.h')


def load(path=HEADER):
    text = open(path, encoding='utf-8').read()
    text = re.sub(r'//[^\n]*', '', text)
    out = {}
    for m in re.finditer(r'constexpr\s+\w+(?:\s+\w+)?\s+(k\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)u?\s*;', text):
        out[m.group(1)] = int(m.group(2), 0)
    for m in re.finditer(r'constexpr\s+\w+\s+(k\w+)\[\d+\]\s*=\s*\{([^}]*)\}\s*;', text):
        out[m.group(1)] = [int(x.strip(), 0) for x in m.group(2).split(',') if x.strip()]
    return out


L = load()

if __name__ == '__main__':
    for k in sorted(L):
        v = L[k]
        print('%-20s %s' % (k, [hex(x) for x in v] if isinstance(v, list) else hex(v)))

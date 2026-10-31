#!/usr/bin/env python3
"""Builds the public release package of Cinematic Combat (CombatMod).

    python tools\\make_release.py [--version 0.9.0-beta] [--no-build] [--force-build] [--smoke] [--strict] [--out DIR]

Output (default CombatMod\\release\\out\\):
    CombatMod_<version>.zip            flat zip: CombatMod.asi, CombatMod.ini, CombatMod.sigs, README.txt, LICENSE.txt,
                                       THIRD_PARTY_NOTICES.txt, CHANGELOG.txt  (extract / drag into the game's bin64)
    CombatMod_<version>.manifest.json  versions, sha256 + size of every file, the .asi hash, every check and its result
    CombatMod_<version>.zip.sha256     sha256 of the zip

What it does:
  1. runs tools\\build.bat when dist\\CombatMod.asi is older than any source file (or with --force-build);
  2. takes CombatMod.asi + CombatMod.sigs from dist\\, and the user-facing files from release\\ (CombatMod.release.ini is the release ini);
  3. verifies (any FAIL = exit code 1, nothing is published to out\\):
       - the release ini: safe release flags (bridge off, Allow* = 0, telemetry off), every [Cinematic] key exists in src\\settings.cpp and is
         in range, every user setting is present, [Advanced] lines are commented and equal the built-in defaults, developer keys commented;
       - no developer-only words / absolute paths / TODO markers in the user-facing text files; version string consistent;
       - the .asi is an x64 DLL without a PDB path, imports only system DLLs; absolute dev paths inside the binary are reported (WARN);
       - the sigs file: parseable, the required signatures present (the optional ones come from src\\gamelayout.h kOptionalSigs), and - when
         the build copies exist under ..\\IDA\\build-<id>\\ - every required signature matches EXACTLY ONCE in each supported build's exe;
       - THIRD_PARTY_NOTICES.txt contains the MinHook licence text verbatim;
       - the finished zip: exact file list (flat, no folders, no dev files), contents byte-identical to the verified inputs, CRC test.
  4. with --smoke also runs tools\\release_smoke.py on the stand-in host (port 6401) against the finished zip.

Offline only: never touches the game folder, never opens port 6400, never launches the game.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..'))            # ...\CombatMod
REPO = os.path.abspath(os.path.join(ROOT, '..'))            # project root
DIST = os.path.join(ROOT, 'dist')
REL = os.path.join(ROOT, 'release')
SRC = os.path.join(ROOT, 'src')

DEFAULT_VERSION = '0.9.0-beta'
# Steam build IDs the release claims to support. Every required signature must match exactly once in each of these (checked when the exe copy exists).
SUPPORTED_BUILDS = ['26308950', '26317560', '26329202']
ZIP_FILES = ['CombatMod.asi', 'CombatMod.ini', 'CombatMod.sigs', 'README.txt', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt', 'CHANGELOG.txt']
TEXT_FILES = {'CombatMod.ini', 'CombatMod.sigs', 'README.txt', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt', 'CHANGELOG.txt'}  # stored with CRLF line ends
USER_DOCS = ['README.txt', 'LICENSE.txt', 'CHANGELOG.txt']   # scanned for developer words (the notices contain licence text only)

# ---- developer-only vocabulary that must not appear in the user-facing documents
DEV_WORDS = [r'\bbridge\b', r'\bAllow(Write|Trace|Watch|Input|Call)\b', r'harness', r'bridge_client', r'telemetry_diff', r'\bselftest\b', r'rawsave',
             r'\bMinHook\b.*\bdetour', r'\betrace\b', r'gamelayout', r'\bIDA\b', r'\bcfg set\b', r'\bskills patch\b', r'tools[\\/]', r'\.cpp\b', r'\.pdb\b',
             r'\bTODO\b', r'\bFIXME\b', r'\bXXX\b', r'\bCONFIRM\b', r'\bTBD\b', r'6400', r'6401']
ABS_PATH = re.compile(r'(?:\b[A-Za-z]:[\\/]|[\\/]Users[\\/]|\bsowar\b|Modding|Crimson-Desert[\\/])')

results = []   # (level, name, detail)


def rec(level, name, detail=''):
    results.append((level, name, detail))
    print('%-5s %s%s' % (level, name, ('  - ' + detail) if detail else ''))


def ok(cond, name, detail='', warn=False):
    rec('PASS' if cond else ('WARN' if warn else 'FAIL'), name, detail if not cond or detail else '')
    return bool(cond)


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def read_text(p):
    with open(p, 'rb') as f:
        raw = f.read()
    return raw.decode('utf-8-sig') if raw.startswith(b'\xef\xbb\xbf') else raw.decode('utf-8')


# ------------------------------------------------------------------ 1. build freshness
def newest_source_mtime():
    newest, who = 0.0, ''
    paths = [os.path.join(ROOT, 'CMakeLists.txt')]
    for base in (SRC, os.path.join(ROOT, 'external')):
        for dp, dn, fn in os.walk(base):
            for f in fn:
                paths.append(os.path.join(dp, f))
    for p in paths:
        m = os.path.getmtime(p)
        if m > newest:
            newest, who = m, p
    return newest, who


def ensure_built(no_build, force):
    asi = os.path.join(DIST, 'CombatMod.asi')
    newest, who = newest_source_mtime()
    stale = (not os.path.exists(asi)) or os.path.getmtime(asi) < newest
    if stale or force:
        if no_build:
            ok(False, 'dist asi is up to date', 'older than %s (rebuild or omit --no-build)' % os.path.relpath(who, ROOT))
            return
        rec('INFO', 'building', 'tools\\build.bat (%s)' % ('forced' if force else 'dist asi older than ' + os.path.relpath(who, ROOT)))
        r = subprocess.run(['cmd', '/c', os.path.join(HERE, 'build.bat')], cwd=ROOT, capture_output=True, text=True)
        tail = '\n'.join((r.stdout + r.stderr).strip().splitlines()[-6:])
        if r.returncode != 0 or 'BUILD OK' not in r.stdout:
            ok(False, 'tools\\build.bat', 'exit %d\n%s' % (r.returncode, tail))
            return
        rec('PASS', 'tools\\build.bat', 'BUILD OK')
    asi = os.path.join(DIST, 'CombatMod.asi')
    newest, who = newest_source_mtime()
    ok(os.path.exists(asi) and os.path.getmtime(asi) >= newest, 'dist\\CombatMod.asi is newer than every source file',
       'newest source: %s' % os.path.relpath(who, ROOT))
    b = os.path.join(ROOT, 'build', 'CombatMod.asi')
    if os.path.exists(b):
        ok(sha256_file(b) == sha256_file(asi), 'dist\\CombatMod.asi == build\\CombatMod.asi', warn=True)


# ------------------------------------------------------------------ 2. settings table of the plugin (src\settings.cpp) and the ini
DEF_RE = re.compile(r'\{"([\w.]+)",\s*"((?:[^"\\]|\\.)*)",\s*CfgType::(\w+),\s*([-\d.]+)f?,\s*([-\d.]+)f?,\s*([-\d.]+)f?,\s*([-\d.]+)f?,\s*(nullptr|"[^"]*"),\s*(true|false)\}')


def load_defs():
    text = read_text(os.path.join(SRC, 'settings.cpp'))
    defs = {}
    for m in DEF_RE.finditer(text):
        key, label, typ, d, lo, hi, step, deflist, adv = m.groups()
        defs[key.lower()] = dict(key=key, type=typ, default=float(d), min=float(lo), max=float(hi), deflist=None if deflist == 'nullptr' else deflist.strip('"'),
                                 advanced=(adv == 'true'))
    return defs


def parse_ini(text):
    """-> (active {section: {key: value}}, commented {section: {key: value}}) ; commented = '; Key=value' lines (no spaces before the key)."""
    active, commented, sec = {}, {}, ''
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line[0] == '[':
            sec = line[1:line.find(']')].strip().lower()
            active.setdefault(sec, {})
            commented.setdefault(sec, {})
            continue
        if line[0] in ';#':
            m = re.match(r'^[;#]\s?([A-Za-z][\w.]*)=(.*)$', line)
            if m:
                commented.setdefault(sec, {})[m.group(1).lower()] = m.group(2).strip()
            continue
        if '=' in line:
            k, v = line.split('=', 1)
            active.setdefault(sec, {})[k.strip().lower()] = v.strip()
    return active, commented


def check_ini(text, defs):
    active, commented = parse_ini(text)
    first = text.splitlines()[0] if text else ''
    ok('=' not in first and first[:1] in ';#', 'ini: first line is a comment without "=" (survives a UTF-8 BOM)', first[:60])
    ok(all(ord(c) < 128 for c in text), 'ini: ASCII only')
    g = active.get('general', {})
    ok(g.get('enabled') == '1', 'ini: [General] Enabled=1')
    ok(g.get('bridgeenabled') == '0', 'ini: developer bridge OFF (BridgeEnabled=0)', g.get('bridgeenabled', 'missing'))
    for k in ('allowwrite', 'allowtrace', 'allowwatch', 'allowinput', 'allowcall'):
        ok(g.get(k) == '0', 'ini: %s=0' % k, g.get(k, 'missing'))
    for k in ('bridgeport', 'enablehooks', 'sampletimecurve', 'initdelayms'):
        ok(k not in g, 'ini: developer key %s is not active' % k, g.get(k, ''))
        ok(k in commented.get('general', {}), 'ini: developer key %s is present (commented)' % k)
    ok(active.get('telemetry', {}).get('enabled') == '0', 'ini: telemetry OFF', active.get('telemetry', {}).get('enabled', 'missing'))
    for sec in ('skills', 'timeamp'):
        ok(not active.get(sec), 'ini: [%s] has no active key' % sec)
    ok(active.get('ui', {}).get('enabled') == '1', 'ini: [UI] Enabled=1')
    ok(active.get('ui', {}).get('startopen') == '0', 'ini: [UI] StartOpen=0')
    ok('testmode' not in active.get('ui', {}) and 'alwayspoll' not in active.get('ui', {}), 'ini: no UI test keys')
    # [Cinematic]: every key valid, every user setting present
    cin = active.get('cinematic', {})
    user_keys = {k for k, d in defs.items() if not d['advanced']}
    ok(len(defs) >= 30, 'settings.cpp table parsed', '%d settings' % len(defs))
    ok(not (user_keys - set(cin)), 'ini: every panel setting is present in [Cinematic]', ','.join(sorted(user_keys - set(cin))))
    bad = []
    for k, v in cin.items():
        d = defs.get(k)
        if not d or d['advanced']:
            bad.append('%s (unknown / advanced key in [Cinematic])' % k)
            continue
        try:
            x = float(v)
        except ValueError:
            bad.append('%s=%s (not a number)' % (k, v))
            continue
        if d['type'] == 'Bool' and x not in (0.0, 1.0):
            bad.append('%s=%s (switch must be 0/1)' % (k, v))
        elif d['type'] != 'Bool' and not (d['min'] <= x <= d['max']):
            bad.append('%s=%s (range %g..%g)' % (k, v, d['min'], d['max']))
    ok(not bad, 'ini: all [Cinematic] values valid and in range', '; '.join(bad))
    # intended first-run defaults (the release contract)
    want = {'slowmo.enabled': 1, 'slowmo.scalemul': 0.5, 'slowmo.rampinmul': 1.6, 'slowmo.rampoutmul': 1.6, 'launch.enabled': 1, 'launch.mult': 2,
            'launch.minvalue': 25, 'hitstop.enabled': 0, 'blindingflash.mode': 0, 'blindingflash.chainspeed': 1, 'reaction.finale': 2, 'reaction.turningslash': 0, 'triggers.towerblast': 1, 'ui.pausegame': 1, 'triggers.enabled': 0}
    diff = ['%s=%s (want %s)' % (k, cin.get(k), v) for k, v in want.items() if k not in cin or abs(float(cin[k]) - v) > 1e-9]
    ok(not diff, 'ini: first-run defaults as specified', '; '.join(diff))
    ok(float(cin.get('blindingflash.chainspeed', 9)) <= 1.0, 'ini: Blinding Flash chain speed is not above 1.0 (it would speed up everyone)', cin.get('blindingflash.chainspeed', 'missing'))
    # [Advanced]: commented, and equal to the built-in default (so "commented" == "in effect")
    adv_active = active.get('advanced', {})
    ok(not adv_active, 'ini: [Advanced] has no active line (all present but commented)', ','.join(adv_active))
    adv_c = commented.get('advanced', {})
    adv_keys = {k for k, d in defs.items() if d['advanced']}
    ok(not (adv_keys - set(adv_c)), 'ini: every [Advanced] key is present (commented)', ','.join(sorted(adv_keys - set(adv_c))))
    mism = []
    for k, v in adv_c.items():
        d = defs.get(k)
        if not d:
            mism.append('%s (unknown)' % k)
        elif d['deflist'] is not None:
            if v.replace(' ', '') != d['deflist']:
                mism.append('%s=%s (code default %s)' % (k, v, d['deflist']))
        elif abs(float(v) - d['default']) > 1e-9:
            mism.append('%s=%s (code default %g)' % (k, v, d['default']))
    ok(not mism, 'ini: commented [Advanced] values == built-in defaults', '; '.join(mism))
    # [Triggers]: no active rule
    trig = active.get('triggers', {})
    live = [k for k, v in trig.items() if 'enabled=0' not in v.replace(' ', '')]
    ok(not live, 'ini: no enabled custom trigger shipped', ','.join(live))
    # justification: every active [Cinematic] key has a comment line above it or within its group
    lines = text.splitlines()
    nocom = []
    for i, ln in enumerate(lines):
        m = re.match(r'^([A-Za-z][\w.]*)=', ln)
        if not m:
            continue
        j = i - 1   # walk up through the contiguous group; it must contain a comment line
        while j >= 0 and lines[j].strip() and not lines[j].lstrip().startswith(';'):
            j -= 1
        if j < 0 or not lines[j].lstrip().startswith(';'):
            nocom.append(m.group(1))
    ok(not nocom, 'ini: every active key has a comment above it', ','.join(nocom))
    return active


# ------------------------------------------------------------------ text checks
def check_text(name, text, version, dev_words=True):
    ok(all(ord(c) < 128 for c in text), '%s: ASCII only' % name, ', '.join(sorted({c for c in text if ord(c) > 127}))[:60])
    m = ABS_PATH.search(text)
    ok(not m, '%s: no absolute paths / user names' % name, m.group(0) if m else '')
    if dev_words:
        hit = [w for w in DEV_WORDS if re.search(w, text, re.I)]
        ok(not hit, '%s: no developer-only words' % name, ', '.join(hit))
    if name in ('README.txt', 'CHANGELOG.txt'):
        ok(version in text, '%s: mentions version %s' % (name, version))


def check_ini_words(text):
    m = ABS_PATH.search(text)
    ok(not m, 'CombatMod.ini: no absolute paths / user names', m.group(0) if m else '')
    hit = [w for w in (r'\bTODO\b', r'\bFIXME\b', r'\bCONFIRM\b', r'harness', r'bridge_client', r'telemetry_diff', r'\.cpp\b') if re.search(w, text, re.I)]
    ok(not hit, 'CombatMod.ini: no developer-only references', ', '.join(hit))


# ------------------------------------------------------------------ the .asi
def pe_info(data):
    if data[:2] != b'MZ':
        return None
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        return None
    machine, nsec, ts, _, _, optsz, chars = struct.unpack_from('<HHIIIHH', data, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    imp_rva, imp_sz = struct.unpack_from('<II', data, dd + 8)
    dbg_rva, dbg_sz = struct.unpack_from('<II', data, dd + 48)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        n, vs, va, rs, ro = struct.unpack_from('<8sIIII', data, so + 40 * i)
        secs.append((va, max(vs, rs), ro))

    def off(rva):
        for va, sz, ro in secs:
            if va <= rva < va + sz:
                return rva - va + ro
        return None
    dlls = []
    if imp_rva:
        o = off(imp_rva)
        while o is not None:
            oft, ts_, fwd, name, ft = struct.unpack_from('<IIIII', data, o)
            if not name:
                break
            no = off(name)
            dlls.append(data[no:data.index(b'\0', no)].decode('ascii', 'replace').lower())
            o += 20
    return dict(machine=machine, dll=bool(chars & 0x2000), magic=magic, timestamp=ts, imports=dlls, debug_dir=(dbg_rva, dbg_sz))


def check_asi(path):
    data = open(path, 'rb').read()
    info = pe_info(data)
    ok(info is not None, 'asi: valid PE file')
    if not info:
        return {}
    ok(info['machine'] == 0x8664 and info['magic'] == 0x20B and info['dll'], 'asi: x64 DLL')
    redist = [d for d in info['imports'] if d.startswith(('vcruntime', 'msvcp', 'concrt', 'vccorlib', 'ucrtbase')) or d.startswith('api-ms-win-crt')]
    ok(not redist, 'asi: no Visual C++ runtime / UCRT import (static CRT)', ', '.join(redist), warn=False)
    ok(b'.pdb' not in data.lower(), 'asi: no PDB path embedded')
    paths = sorted({m.group(0).decode('ascii', 'replace') for m in re.finditer(rb'[A-Za-z]:\\[\x20-\x7e]{3,}', data)})
    ok(not paths, 'asi: no absolute (developer) paths in the binary', '%d strings, e.g. %s' % (len(paths), paths[0][:70] if paths else ''), warn=True)
    return dict(imports=info['imports'], pe_timestamp=info['timestamp'], dev_path_strings=len(paths))



def section_hashes(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec, = struct.unpack_from('<H', d, pe + 6)
    optsz, = struct.unpack_from('<H', d, pe + 20)
    out = {}
    for i in range(nsec):
        n, vs, va, rs, ro = struct.unpack_from('<8sIIII', d, pe + 24 + optsz + 40 * i)
        out[n.rstrip(b'\0').decode()] = sha256_bytes(d[ro:ro + rs])
    return out


def verify_source_build():
    """Rebuilds the plugin from a COPY of the sources in a temp folder (the real dist/ and build/ are not touched) and compares the code
    section (.text) with dist/CombatMod.asi: equal = the shipped binary was built from exactly these sources (timestamps differ, code does not)."""
    import shutil
    import tempfile
    tmp = tempfile.mkdtemp(prefix='combatmod_verify_')
    try:
        dst = os.path.join(tmp, 'CombatMod')
        ign = shutil.ignore_patterns('build', 'deployed_backup', 'sessions', 'snapshots', 'release', '__pycache__', '*.pyc', 'tools')
        shutil.copytree(ROOT, dst, ignore=ign)
        os.makedirs(os.path.join(dst, 'tools'))
        shutil.copy2(os.path.join(HERE, 'build.bat'), os.path.join(dst, 'tools', 'build.bat'))
        r = subprocess.run(['cmd', '/c', os.path.join(dst, 'tools', 'build.bat')], cwd=dst, capture_output=True, text=True)
        built = os.path.join(dst, 'build', 'CombatMod.asi')
        if r.returncode != 0 or not os.path.exists(built):
            ok(False, 'verify-source: independent rebuild', (r.stdout + r.stderr)[-300:])
            return
        a, b = section_hashes(os.path.join(DIST, 'CombatMod.asi')), section_hashes(built)
        ok(a.get('.text') == b.get('.text') and a.get('.pdata') == b.get('.pdata'), 'verify-source: .text/.pdata of dist/CombatMod.asi == an independent rebuild of the current sources',
           'dist .text %s, rebuild %s' % (a.get('.text', '-')[:12], b.get('.text', '-')[:12]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ------------------------------------------------------------------ sigs
def check_sigs(path, skip_exe):
    text = read_text(path)
    ok(all(ord(c) < 128 for c in text), 'sigs: ASCII only')
    m = ABS_PATH.search(text)
    ok(not m, 'sigs: no absolute paths', m.group(0) if m else '')
    names = []
    for ln in text.splitlines():
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        name, rest = ln.split('=', 1)
        pat = rest.split('|')[0].split()
        good = all(t == '??' or t == '?' or re.fullmatch(r'[0-9A-Fa-f]{2}', t) for t in pat)
        fixed = sum(1 for t in pat if t not in ('??', '?'))
        ok(good and fixed >= 24, 'sigs: %s parses, %d fixed bytes' % (name.strip(), fixed))
        names.append(name.strip())
    ok(len(names) == len(set(names)) and len(names) >= 4, 'sigs: %d signatures, names unique' % len(names))
    gl = read_text(os.path.join(SRC, 'gamelayout.h'))
    om = re.search(r'kOptionalSigs\[\]\s*=\s*\{([^}]*)\}', gl)
    optional = re.findall(r'"(\w+)"', om.group(1)) if om else []
    required = [n for n in names if n not in optional]
    # the plugin looks these up by name: if one is renamed in the file the feature silently never installs
    src_all = ''.join(read_text(os.path.join(SRC, f)) for f in os.listdir(SRC) if f.endswith('.cpp') and f != 'selftest.cpp')
    used = [n for n in names if re.search(r'"%s"' % n, src_all)]
    ok(set(used) >= set(required), 'sigs: every required signature is looked up by the plugin source', 'unused: %s' % ','.join(set(required) - set(used)))
    ok(set(optional) <= set(names), 'sigs: optional signatures are a subset of the file', ','.join(set(optional) - set(names)))
    rec('INFO', 'sigs: required = %s' % ', '.join(required), 'optional = %s' % ', '.join(optional))
    per_build = {}
    if skip_exe:
        return names, required, per_build
    script = os.path.join(REPO, 'Combat-Mod-Research', 'scripts', 'verify_sigs.py')
    for b in SUPPORTED_BUILDS:
        exe = os.path.join(REPO, 'IDA', 'build-%s' % b, 'CrimsonDesert.exe')
        if not (os.path.exists(exe) and os.path.exists(script)):
            rec('WARN', 'sigs vs build %s' % b, 'exe copy not found under IDA\\ - signature uniqueness NOT verified for this build')
            per_build[b] = None
            continue
        r = subprocess.run([sys.executable, script, path, '--exe', exe, '--json'], capture_output=True, text=True, cwd=os.path.dirname(script))
        start = r.stdout.find('\n{')
        try:
            data, _ = json.JSONDecoder().raw_decode(r.stdout[start + 1:])   # header lines come before and a summary after the JSON
        except ValueError:
            ok(False, 'sigs vs build %s' % b, 'verify_sigs.py output not parseable (exit %d): %s' % (r.returncode, (r.stderr or r.stdout)[-300:]))
            continue
        rows = {x['name']: x for x in data['rows']}
        bad = [n for n in required if rows.get(n, {}).get('status') != 'UNIQUE']
        ok(not bad, 'sigs vs build %s: every required signature matches exactly once' % b, 'not unique: %s' % ', '.join(bad))
        per_build[b] = dict(exe_sha256=data.get('sha256'), unique=sorted(n for n, x in rows.items() if x['status'] == 'UNIQUE'),
                            not_unique={n: x['status'] for n, x in rows.items() if x['status'] != 'UNIQUE'})
    return names, required, per_build


# ------------------------------------------------------------------ licence notice
def check_notices(text):
    t = read_text(os.path.join(ROOT, 'external', 'minhook', 'include', 'MinHook.h'))
    body = re.match(r'\s*/\*(.*?)\*/', t, re.S).group(1)
    body = '\n'.join(re.sub(r'^\s*\* ?', '', l).rstrip() for l in body.split('\n')).strip('\n')
    norm = lambda s: re.sub(r'\s+', ' ', s)
    ok(norm(body) in norm(text), 'notices: MinHook BSD-2-Clause text is verbatim from external\\minhook')
    h = read_text(os.path.join(ROOT, 'external', 'minhook', 'src', 'hde', 'hde64.c'))
    ok('Vyacheslav Patkov' in text and 'Vyacheslav Patkov' in h and 'Tsuda Kageyu' in text, 'notices: MinHook and HDE64 copyright holders named')


# ------------------------------------------------------------------ zip
def crlf(b):
    return b.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')


def build_zip(out_zip, files, stamp):
    dt = datetime.datetime.fromtimestamp(stamp, datetime.timezone.utc)
    zdt = (max(dt.year, 1980), dt.month, dt.day, dt.hour, dt.minute, dt.second - dt.second % 2)
    with zipfile.ZipFile(out_zip, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in ZIP_FILES:
            zi = zipfile.ZipInfo(name, zdt)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o100644 << 16
            zi.create_system = 0
            z.writestr(zi, files[name], compresslevel=9)


def verify_zip(path, files):
    with zipfile.ZipFile(path) as z:
        ok(z.testzip() is None, 'zip: CRC test')
        names = z.namelist()
        ok(names == ZIP_FILES, 'zip: exact flat layout (no folders)', ', '.join(names))
        ok(not [n for n in names if '/' in n or '\\' in n or n.endswith('/')], 'zip: nothing in a sub-folder')
        bad = [n for n in ZIP_FILES if z.read(n) != files[n]]
        ok(not bad, 'zip: contents identical to the verified inputs', ', '.join(bad))
        forbidden = [n for n in names if re.search(r'(\.log|\.dev\.ini|\.cinematic\.ini|\.pdb|\.py|\.bat|\.pyc|telemetry|\.csv|\.json)$', n, re.I)]
        ok(not forbidden, 'zip: no developer / generated files', ', '.join(forbidden))
        return {n: dict(size=z.getinfo(n).file_size, compressed=z.getinfo(n).compress_size) for n in names}


# ------------------------------------------------------------------ main
def git_info():
    try:
        r = subprocess.run(['git', '-C', REPO, 'rev-parse', 'HEAD'], capture_output=True, text=True)
        c = r.stdout.strip() if r.returncode == 0 else None
        d = subprocess.run(['git', '-C', REPO, 'status', '--porcelain', '--', 'CombatMod/src', 'CombatMod/release', 'CombatMod/dist'], capture_output=True, text=True)
        return dict(commit=c, dirty_paths=len(d.stdout.strip().splitlines()) if d.returncode == 0 else None)
    except OSError:
        return dict(commit=None, dirty_paths=None)


def main():
    global REL
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', default=DEFAULT_VERSION)
    ap.add_argument('--release-dir', default=REL, help='folder with the user-facing templates (default CombatMod\release)')
    ap.add_argument('--out', default=os.path.join(REL, 'out'))
    ap.add_argument('--no-build', action='store_true', help='never run build.bat; FAIL when the dist asi is stale')
    ap.add_argument('--force-build', action='store_true')
    ap.add_argument('--skip-exe-check', action='store_true', help='do not scan the IDA\\build-<id> exe copies for the signatures')
    ap.add_argument('--verify-source', action='store_true', help='rebuild a COPY of the sources and compare the code section with dist/CombatMod.asi (about 1-2 minutes)')
    ap.add_argument('--smoke', action='store_true', help='run tools\\release_smoke.py (stand-in host, port 6401) on the finished zip')
    ap.add_argument('--strict', action='store_true', help='WARN counts as FAIL')
    a = ap.parse_args()
    ver = a.version
    REL = os.path.abspath(a.release_dir)
    ok(re.fullmatch(r'\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?', ver) is not None, 'version string "%s" is semver-like' % ver)

    ensure_built(a.no_build, a.force_build)
    sources = {
        'CombatMod.asi': os.path.join(DIST, 'CombatMod.asi'),
        'CombatMod.sigs': os.path.join(DIST, 'CombatMod.sigs'),
        'CombatMod.ini': os.path.join(REL, 'CombatMod.release.ini'),
        'README.txt': os.path.join(REL, 'README.txt'),
        'LICENSE.txt': os.path.join(REL, 'LICENSE.txt'),
        'THIRD_PARTY_NOTICES.txt': os.path.join(REL, 'THIRD_PARTY_NOTICES.txt'),
        'CHANGELOG.txt': os.path.join(REL, 'CHANGELOG.txt'),
    }
    missing = [n for n, p in sources.items() if not os.path.exists(p)]
    if missing:
        ok(False, 'input files exist', ', '.join(missing))
        return finish(a, ver, None, None, {})
    ok(sha256_file(sources['CombatMod.ini']) != sha256_file(os.path.join(DIST, 'CombatMod.dev.ini')), 'release ini is not the developer ini')
    ok('CombatMod.dev' not in ZIP_FILES, 'the developer marker file CombatMod.dev is not packaged (without it BridgeEnabled=1 is ignored)')
    vh = read_text(os.path.join(SRC, 'version.h'))
    mv = re.search(r'#define\s+COMBATMOD_VERSION\s+"([^"]+)"', vh)
    ok(mv is not None and mv.group(1) == ver, 'src\\version.h COMBATMOD_VERSION matches the release version', (mv.group(1) if mv else 'missing') + ' vs ' + ver)

    defs = load_defs()
    ini_text = read_text(sources['CombatMod.ini'])
    check_ini(ini_text, defs)
    check_ini_words(ini_text)
    for n in USER_DOCS:
        check_text(n, read_text(sources[n]), ver)
    check_text('THIRD_PARTY_NOTICES.txt', read_text(sources['THIRD_PARTY_NOTICES.txt']), ver, dev_words=False)
    check_notices(read_text(sources['THIRD_PARTY_NOTICES.txt']))
    # the README documents every panel key of the ini
    readme = read_text(sources['README.txt'])
    miss = [d['key'] for d in defs.values() if not d['advanced'] and d['key'] not in readme]
    ok(not miss, 'README: settings table covers every [Cinematic] key', ', '.join(miss), warn=True)
    asi_info = check_asi(sources['CombatMod.asi'])
    if a.verify_source:
        verify_source_build()
    names, required, per_build = check_sigs(sources['CombatMod.sigs'], a.skip_exe_check)

    fails = [r for r in results if r[0] == 'FAIL' or (a.strict and r[0] == 'WARN')]
    if fails:
        rec('FAIL', 'not packaging', '%d failed check(s)' % len(fails))
        return finish(a, ver, None, None, dict(asi=asi_info, per_build=per_build, sources=sources))

    files = {}
    for n in ZIP_FILES:
        b = open(sources[n], 'rb').read()
        if b.startswith(b'\xef\xbb\xbf'):
            b = b[3:]
        files[n] = crlf(b) if n in TEXT_FILES else b
    os.makedirs(a.out, exist_ok=True)
    zpath = os.path.join(a.out, 'CombatMod_%s.zip' % ver)
    stamp = os.path.getmtime(sources['CombatMod.asi'])
    build_zip(zpath, files, stamp)
    zinfo = verify_zip(zpath, files)
    return finish(a, ver, zpath, files, dict(asi=asi_info, per_build=per_build, sources=sources, zinfo=zinfo, required=required))


def finish(a, ver, zpath, files, extra):
    nfail = sum(1 for r in results if r[0] == 'FAIL')
    nwarn = sum(1 for r in results if r[0] == 'WARN')
    if zpath and nfail == 0:
        asi_hash = sha256_bytes(files['CombatMod.asi'])
        manifest = dict(
            name='Cinematic Combat (CombatMod)', version=ver,
            built_utc=datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
            asi=dict(sha256=asi_hash, size=len(files['CombatMod.asi']), file_mtime_utc=datetime.datetime.fromtimestamp(os.path.getmtime(extra['sources']['CombatMod.asi']),
                                                                                                                   datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
                     **extra['asi']),
            supported_builds=SUPPORTED_BUILDS, required_signatures=extra['required'], signature_check_per_build=extra['per_build'],
            files={n: dict(sha256=sha256_bytes(files[n]), size=len(files[n]), source=os.path.relpath(extra['sources'][n], ROOT)) for n in ZIP_FILES},
            zip=dict(name=os.path.basename(zpath), sha256=sha256_file(zpath), size=os.path.getsize(zpath), entries=extra['zinfo']),
            git=git_info(), checks=dict(pass_=sum(1 for r in results if r[0] == 'PASS'), warn=nwarn, fail=nfail),
            warnings=[dict(name=r[1], detail=r[2]) for r in results if r[0] == 'WARN'])
        mp = os.path.join(a.out, 'CombatMod_%s.manifest.json' % ver)
        with open(mp, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(manifest, f, indent=2)
            f.write('\n')
        with open(zpath + '.sha256', 'w', newline='\n') as f:
            f.write('%s *%s\n' % (manifest['zip']['sha256'], os.path.basename(zpath)))
        print('\nasi sha256 %s\nzip  sha256 %s\n%s\n%s' % (asi_hash, manifest['zip']['sha256'], zpath, mp))
    print('\n%d passed, %d warning(s), %d FAILED' % (sum(1 for r in results if r[0] == 'PASS'), nwarn, nfail))
    if nfail:
        return 1
    if a.smoke and zpath:
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'release_smoke.py'), zpath], cwd=ROOT)
        return r.returncode
    return 0


if __name__ == '__main__':
    sys.exit(main())

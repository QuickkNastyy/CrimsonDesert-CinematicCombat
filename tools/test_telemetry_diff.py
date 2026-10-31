"""Unit test for telemetry_diff.py with two synthetic session files (run: python -I tools/test_telemetry_diff.py)."""
import copy
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import telemetry_diff as td

FP = "11" * 24


def session(stamp="0x6AB28F00", size=33554432):
    return {
        "schema": 1, "plugin": "CombatMod test",
        "module": {"exe": "x.exe", "time_date_stamp": stamp, "size_of_image": size, "file_size": 1000, "base": "0x140000000"},
        "signatures": [
            {"name": "time_curve_start", "status": "RESOLVED", "matches": 1, "rva": "0x1000", "match_rva": "0x1000", "fingerprint": FP},
            {"name": "apply_attack_skill_to_position", "status": "RESOLVED", "matches": 1, "rva": "0x2000", "match_rva": "0x2000", "fingerprint": "22" * 24},
            {"name": "event_dispatcher", "status": "RESOLVED", "matches": 1, "rva": "0x3000", "match_rva": "0x3000", "fingerprint": "33" * 24},
            {"name": "time_mgr_global", "status": "RESOLVED", "matches": 1, "rva": "0x9000", "match_rva": "0x8000", "fingerprint": "44" * 24},
        ],
        "hooks": [{"name": "time_curve_start", "rva": "0x1000", "state": "installed", "fingerprint": FP}],
        "event_table": {"a1": "0xA000", "slots": [
            {"type": 3, "fn_rva": "0x5000", "fingerprint": "aa" * 24},
            {"type": 123, "fn_rva": "0x1000", "fingerprint": FP},
            {"type": 200, "fn_rva": "0x7000", "fingerprint": "cc" * 24}]},
        "events": [
            {"type": 3, "fn_rva": "0x5000", "fingerprint": "aa" * 24, "count": 9, "sample_d": "0300" + "00" * 94, "sample_floats": [0] * 6},
            {"type": 123, "fn_rva": "0x1000", "fingerprint": FP, "count": 4, "sample_d": "7b00" + "00" * 94, "sample_floats": [0] * 6}],
        "skill_keys": [{"key": 10253, "count": 5, "player_count": 5}, {"key": 1002, "count": 2, "player_count": 2}],
        "launch_records": [{"id": "0xAAAA", "keys": [1021, 0, 0, 0], "orig_strength": [15, 15], "orig_angles": [0.436332, 0.436332], "angle_ok": True, "calls": 3, "player": True,
                            "marker": ["0x7F7FFFFF"] * 3, "s_probe": "00" * 448}],
        "slowmo_layouts": [{"scale": 0.6, "a": 0.5, "b": 0.2, "count": 7, "modified": 5, "filtered": 2, "keys": [{"key": 10253, "count": 7}]},
                           {"scale": 0.99, "a": 0.0333, "b": 0.0333, "count": 30, "modified": 0, "filtered": 0, "keys": [{"key": 0, "count": 30}]}],
        "hitstop_per_minute": [],
    }


def texts(findings, sev=None):
    return [t for s, t in findings if sev is None or s == sev]


def has(findings, sev, *needles):
    return any(s == sev and all(n in t for n in needles) for s, t in findings)


def main():
    a = session()
    # 1. identical files: no critical findings, no changes
    f = td.diff(a, copy.deepcopy(a))
    assert not texts(f, "CRITICAL") and not texts(f, "CHANGE"), f
    assert has(f, "info", "same game build"), f

    # 2. a patched build
    b = session(stamp="0x6AC00000", size=34000000)
    b["signatures"][0]["status"] = "MISSING"; b["signatures"][0]["matches"] = 0; b["signatures"][0]["rva"] = None   # lost
    b["signatures"][1]["rva"] = "0x2100"                                                                              # moved
    b["signatures"][2]["fingerprint"] = "99" * 24                                                                     # code changed under the same pattern
    b["hooks"][0]["fingerprint"] = "77" * 24
    b["event_table"]["slots"][0]["fn_rva"] = "0x5400"                                                                 # handler moved
    b["event_table"]["slots"][1]["fingerprint"] = "55" * 24                                                           # type 123 handler code changed
    del b["event_table"]["slots"][2]                                                                                  # type 200 removed (never fired)
    b["event_table"]["slots"].append({"type": 300, "fn_rva": "0x8000", "fingerprint": "dd" * 24})                     # new type
    b["events"][1]["fingerprint"] = "55" * 24
    b["skill_keys"] = [{"key": 10253, "count": 1, "player_count": 1}, {"key": 5555, "count": 1, "player_count": 0}]
    b["launch_records"][0]["s_probe"] = "00" * 100 + "ff" * 4 + "00" * (448 - 104)                                       # layout bytes changed
    b["launch_records"].append({"id": "0xBBBB", "keys": [7, 0, 0, 0], "orig_strength": [9, 9], "orig_angles": [0.1, 0.1], "angle_ok": True, "calls": 1, "player": False, "marker": ["0x7F7FFFFF"] * 3, "s_probe": None})
    b["slowmo_layouts"][0]["keys"] = [{"key": 999, "count": 7}]
    b["slowmo_layouts"].append({"scale": 0.3, "a": 0.4, "b": 0.4, "count": 1, "modified": 1, "filtered": 0, "keys": []})
    f = td.diff(a, b)
    assert has(f, "CHANGE", "DIFFERENT game build"), f
    assert has(f, "CRITICAL", "signature time_curve_start LOST"), f
    assert has(f, "CHANGE", "signature apply_attack_skill_to_position moved", "0x2000 -> 0x2100", "+256"), f
    assert has(f, "CRITICAL", "signature event_dispatcher", "CODE at the match changed"), f
    assert has(f, "CRITICAL", "hooked function time_curve_start", "prologue bytes changed"), f
    assert has(f, "CHANGE", "event type 3: handler moved 0x5000 -> 0x5400"), f
    assert has(f, "CRITICAL", "event type 123: the handler CODE changed", "type 123 is the time-scale event"), f
    assert has(f, "CHANGE", "event type 200 is GONE") and not has(f, "CRITICAL", "event type 200"), f
    assert has(f, "CHANGE", "event type 300 is NEW"), f
    assert has(f, "CHANGE", "skill keys only in the first file", "1002") and has(f, "info", "skill keys only in the second file", "5555"), f
    assert has(f, "CHANGE", "launch record 0xAAAA: S layout bytes differ at +0x64(4)"), f
    assert has(f, "CHANGE", "launch record 0xBBBB is new"), f
    assert has(f, "CHANGE", "slow-motion layout (scale 0.6, A 0.5, B 0.2): triggering skill keys changed"), f
    assert has(f, "CHANGE", "slow-motion layout (scale 0.3, A 0.4, B 0.4) is NEW"), f
    rep = td.report(f)
    assert rep.index("[CRITICAL]") < rep.index("[CHANGE]") < rep.index("[info]") and "TRIAGE: 4 critical" in rep, rep

    # 3. an event type that FIRED before and is gone is critical
    c = session()
    c["event_table"]["slots"] = [s for s in c["event_table"]["slots"] if s["type"] != 3]
    c["events"] = [e for e in c["events"] if e["type"] != 3]
    assert has(td.diff(a, c), "CRITICAL", "event type 3 is GONE", "fired in the old session"), "gone+fired"

    # 3b. the time-scale event type id is build specific: each file's "layout" section names it (123 on the old build, 125 on the new one)
    la, lb = session(), session()
    la["layout"] = {"build": 25477059, "time_event_type": 123, "ev_scale": 8}
    lb["layout"] = {"build": 26308950, "time_event_type": 125, "ev_scale": 12}
    for sl in lb["event_table"]["slots"]:
        if sl["type"] == 123:
            sl["type"] = 125
            sl["fingerprint"] = "77" * 24
    for e in lb["events"]:
        if e["type"] == 123:
            e["type"] = 125
            e["fingerprint"] = "77" * 24
    f = td.diff(la, lb)
    assert has(f, "CHANGE", "build layout constants differ", "ev_scale 8 -> 12", "time_event_type 123 -> 125"), f

    # 4. the command line: exit codes with --strict, output format
    with tempfile.TemporaryDirectory() as d:
        pa, pb = os.path.join(d, "a.json"), os.path.join(d, "b.json")
        json.dump(a, open(pa, "w"))
        json.dump(b, open(pb, "w"))
        out = subprocess.run([sys.executable, "-I", os.path.join(HERE, "telemetry_diff.py"), pa, pb, "--strict"], capture_output=True, text=True)
        assert out.returncode == 1 and "TRIAGE: 4 critical" in out.stdout, out
        out = subprocess.run([sys.executable, "-I", os.path.join(HERE, "telemetry_diff.py"), pa, pa, "--strict", "--all"], capture_output=True, text=True)
        assert out.returncode == 0 and "TRIAGE: 0 critical, 0 changes" in out.stdout and "counts:" in out.stdout, out
    print("telemetry_diff: all tests passed")


if __name__ == "__main__":
    main()

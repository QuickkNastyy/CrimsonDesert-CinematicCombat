#!/usr/bin/env python3
"""Compare two CombatMod telemetry session files (same or different game build) and print the post-patch triage report.

  python telemetry_diff.py OLD.json NEW.json [--strict] [--all]

Sections: build identity, signatures (lost / newly resolved / moved / code changed), our hooks, event handlers (fn RVA or code fingerprint
changed, event types added / removed), skill keys, launch records (new / removed, S-record layout changes), slow-motion layouts.
Findings are tagged  [CRITICAL]  (something we depend on is gone or changed: fix before shipping), [CHANGE] (differs, review) or [info].
--strict makes the exit code 1 when any [CRITICAL] finding exists; --all also lists unchanged items' counts.
"""
import json
import sys


def hexval(x):
    """'0x1A' -> 26; None/'abs:..' stay as-is for comparison."""
    if isinstance(x, str) and x.startswith("0x"):
        return int(x, 16)
    return x


def fmt(x):
    return "-" if x is None else (hex(x) if isinstance(x, int) else str(x))


def ranges(a, b):
    """Byte ranges (offset, length) where two equal-length hex strings differ."""
    out, i, n = [], 0, min(len(a), len(b)) // 2
    while i < n:
        if a[2 * i:2 * i + 2] != b[2 * i:2 * i + 2]:
            j = i
            while j < n and a[2 * j:2 * j + 2] != b[2 * j:2 * j + 2]:
                j += 1
            out.append((i, j - i))
            i = j
        else:
            i += 1
    return out


def diff(a, b, show_all=False):
    """Returns a list of (severity, text) tuples; severity in CRITICAL / CHANGE / info."""
    r = []
    add = lambda sev, text: r.append((sev, text))

    # ---- build identity
    ma, mb = a.get("module", {}), b.get("module", {})
    same_build = ma.get("time_date_stamp") == mb.get("time_date_stamp") and ma.get("size_of_image") == mb.get("size_of_image") and ma.get("file_size") == mb.get("file_size")
    if same_build:
        add("info", "same game build (TimeDateStamp %s, SizeOfImage %s, file size %s)" % (ma.get("time_date_stamp"), ma.get("size_of_image"), ma.get("file_size")))
    else:
        add("CHANGE", "DIFFERENT game build: TimeDateStamp %s -> %s, SizeOfImage %s -> %s, file size %s -> %s" % (
            ma.get("time_date_stamp"), mb.get("time_date_stamp"), ma.get("size_of_image"), mb.get("size_of_image"), ma.get("file_size"), mb.get("file_size")))
    if a.get("plugin") != b.get("plugin"):
        add("info", "plugin build differs: %r -> %r" % (a.get("plugin"), b.get("plugin")))

    # ---- signatures
    sa = {s["name"]: s for s in a.get("signatures", [])}
    sb = {s["name"]: s for s in b.get("signatures", [])}
    for name in sorted(set(sa) | set(sb)):
        x, y = sa.get(name), sb.get(name)
        if x is None:
            add("info", "signature %s: new in the second file (%s)" % (name, y["status"]))
        elif y is None:
            add("CHANGE", "signature %s: no longer in the sigs file" % name)
        else:
            if x["status"] == "RESOLVED" and y["status"] != "RESOLVED":
                add("CRITICAL", "signature %s LOST: %s -> %s (%d matches); every feature that needs it is inert" % (name, x["status"], y["status"], y["matches"]))
            elif x["status"] != "RESOLVED" and y["status"] == "RESOLVED":
                add("CHANGE", "signature %s now resolves (was %s)" % (name, x["status"]))
            elif x["status"] == "RESOLVED":
                ra, rb = hexval(x.get("rva")), hexval(y.get("rva"))
                if ra != rb:
                    add("CHANGE", "signature %s moved: RVA %s -> %s (%+d bytes)%s" % (name, fmt(ra), fmt(rb), (rb - ra) if isinstance(ra, int) and isinstance(rb, int) else 0, "" if same_build else "  (expected after a patch)"))
                if x.get("fingerprint") and y.get("fingerprint") and x["fingerprint"] != y["fingerprint"]:
                    add("CRITICAL", "signature %s: the CODE at the match changed (first 24 bytes differ) although the pattern still matches: %s -> %s" % (name, x["fingerprint"], y["fingerprint"]))
                if x.get("matches") != y.get("matches"):
                    add("CHANGE", "signature %s: match count %s -> %s" % (name, x.get("matches"), y.get("matches")))

    # ---- hooks
    ha = {h["name"]: h for h in a.get("hooks", [])}
    hb = {h["name"]: h for h in b.get("hooks", [])}
    for name in sorted(set(ha) | set(hb)):
        x, y = ha.get(name), hb.get(name)
        if x and y:
            if x.get("fingerprint") != y.get("fingerprint"):
                add("CRITICAL", "hooked function %s: prologue bytes changed (%s -> %s); re-check the detour signature/arguments" % (name, x.get("fingerprint"), y.get("fingerprint")))
            if hexval(x.get("rva")) != hexval(y.get("rva")):
                add("CHANGE", "hooked function %s moved: RVA %s -> %s" % (name, fmt(hexval(x.get("rva"))), fmt(hexval(y.get("rva")))))
        elif x and not y:
            add("CHANGE", "hook %s was installed before but not in the second file" % name)
        elif y and not x:
            add("info", "hook %s is new in the second file" % name)

    # ---- event handlers: union of the dispatcher table (all registered types) and the types that actually fired
    def handlers(d):
        out = {}
        for s in d.get("event_table", {}).get("slots", []):
            out[s["type"]] = {"fn": s.get("fn_rva"), "fp": s.get("fingerprint"), "seen": False}
        for e in d.get("events", []):
            h = out.setdefault(e["type"], {"fn": e.get("fn_rva"), "fp": e.get("fingerprint"), "seen": False})
            h["seen"] = True
            h["count"] = e.get("count")
            h["d"] = e.get("sample_d")
        return out
    ea, eb = handlers(a), handlers(b)
    # the time-scale event type id is build specific (123 on build 25477059, 125 on 26308950): each file says which one it used ("layout" section; older files = 123)
    time_types = {a.get("layout", {}).get("time_event_type", 123), b.get("layout", {}).get("time_event_type", 123)}
    la_, lb_ = a.get("layout", {}), b.get("layout", {})
    if la_ != lb_:
        keys_ = sorted(set(la_) | set(lb_))
        add("CHANGE", "build layout constants differ (src/gamelayout.h): " + ", ".join("%s %s -> %s" % (k, la_.get(k), lb_.get(k)) for k in keys_ if la_.get(k) != lb_.get(k))[:600])
    for t in sorted(set(ea) | set(eb)):
        x, y = ea.get(t), eb.get(t)
        if x is None:
            add("CHANGE", "event type %d is NEW (handler %s)" % (t, y["fn"]))
        elif y is None:
            add("CRITICAL" if x["seen"] else "CHANGE", "event type %d is GONE (it was handled at %s%s)" % (t, x["fn"], ", and it fired in the old session" if x["seen"] else ""))
        else:
            if x["fn"] != y["fn"]:
                add("CHANGE", "event type %d: handler moved %s -> %s%s" % (t, x["fn"], y["fn"], "" if same_build else "  (expected after a patch; check the fingerprint)"))
            if x["fp"] and y["fp"] and x["fp"] != y["fp"]:
                add("CRITICAL" if t in time_types else "CHANGE", "event type %d: the handler CODE changed (fingerprint %s -> %s)%s" % (t, x["fp"], y["fp"], "  <- type %d is the time-scale event our slow motion hooks" % t if t in time_types else ""))
            if x.get("d") and y.get("d") and x["d"][:4] != y["d"][:4]:
                add("info", "event type %d: sample data differs at the type field (%s vs %s)" % (t, x["d"][:4], y["d"][:4]))

    # ---- skill keys
    ka = {k["key"] for k in a.get("skill_keys", [])}
    kb = {k["key"] for k in b.get("skill_keys", [])}
    if ka - kb:
        add("CHANGE", "skill keys only in the first file (%d): %s" % (len(ka - kb), ", ".join(str(k) for k in sorted(ka - kb)[:40])))
    if kb - ka:
        add("info", "skill keys only in the second file (%d): %s" % (len(kb - ka), ", ".join(str(k) for k in sorted(kb - ka)[:40])))

    # ---- launch records, matched by id (keys + original strength)
    la = {x["id"]: x for x in a.get("launch_records", [])}
    lb = {x["id"]: x for x in b.get("launch_records", [])}
    for i in sorted(set(la) | set(lb)):
        x, y = la.get(i), lb.get(i)
        if x is None:
            add("CHANGE", "launch record %s is new: keys %s, strength %s, angles %s" % (i, y["keys"], y["orig_strength"], y["orig_angles"]))
        elif y is None:
            add("CHANGE", "launch record %s disappeared (keys %s, strength %s)" % (i, x["keys"], x["orig_strength"]))
        else:
            if x.get("marker") != y.get("marker"):
                add("CRITICAL", "launch record %s: the marker dwords of the record changed %s -> %s (the launch boost no longer recognises it)" % (i, x.get("marker"), y.get("marker")))
            if x.get("angle_ok") != y.get("angle_ok"):
                add("CHANGE", "launch record %s: angle fields now %s" % (i, "sane" if y.get("angle_ok") else "not sane"))
            if x.get("s_probe") and y.get("s_probe") and x["s_probe"] != y["s_probe"]:
                rs = ranges(x["s_probe"], y["s_probe"])
                add("CHANGE", "launch record %s: S layout bytes differ at %s" % (i, ", ".join("+0x%X(%d)" % rg for rg in rs[:12]) + (" ..." if len(rs) > 12 else "")))

    # ---- slow-motion layouts: (scale, A, B)
    def slow(d):
        return {(round(s["scale"], 5), round(s["a"], 5), round(s["b"], 5)): s for s in d.get("slowmo_layouts", [])}
    wa, wb = slow(a), slow(b)
    for k in sorted(set(wa) | set(wb)):
        x, y = wa.get(k), wb.get(k)
        if x is None:
            add("CHANGE", "slow-motion layout (scale %g, A %g, B %g) is NEW" % k)
        elif y is None:
            add("CHANGE", "slow-motion layout (scale %g, A %g, B %g) is gone (it fired %d times before)" % (k + (x["count"],)))
        else:
            ka_, kb_ = {q["key"] for q in x.get("keys", [])}, {q["key"] for q in y.get("keys", [])}
            if ka_ != kb_:
                add("CHANGE", "slow-motion layout (scale %g, A %g, B %g): triggering skill keys changed: %s -> %s" % (k + (sorted(ka_), sorted(kb_))))
    if show_all:
        add("info", "counts: signatures %d/%d, handlers %d/%d, skill keys %d/%d, launch records %d/%d, slow-mo layouts %d/%d" % (
            len(sa), len(sb), len(ea), len(eb), len(ka), len(kb), len(la), len(lb), len(wa), len(wb)))
    return r


def report(findings):
    order = {"CRITICAL": 0, "CHANGE": 1, "info": 2}
    lines = []
    for sev, text in sorted(findings, key=lambda f: order[f[0]]):
        lines.append("[%s] %s" % (sev, text))
    n = {k: sum(1 for s, _ in findings if s == k) for k in order}
    lines.append("")
    lines.append("TRIAGE: %d critical, %d changes, %d notes" % (n["CRITICAL"], n["CHANGE"], n["info"]))
    return "\n".join(lines)


def main(argv):
    args = [x for x in argv if not x.startswith("--")]
    if len(args) != 2:
        print(__doc__)
        return 2
    a = json.load(open(args[0], encoding="utf-8"))
    b = json.load(open(args[1], encoding="utf-8"))
    f = diff(a, b, "--all" in argv)
    print("OLD: %s\nNEW: %s\n" % (args[0], args[1]))
    print(report(f))
    return 1 if ("--strict" in argv and any(s == "CRITICAL" for s, _ in f)) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

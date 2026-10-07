"""Text table of a dynsweep result (``dynsweep.json``). No dependency on the core module, so the summary printer can use it
without loading ``sawblade_core``."""
from __future__ import annotations


def _f(v, fmt="{:.2f}") -> str:
    return "n/a" if v is None else fmt.format(v)


def format_table(r: dict) -> str:
    offs = r["inputOffsetsDb"]
    lines = [f"dynsweep: {r['result']} (gate {'on' if r['gateEnabled'] else 'off'}, bus comp {'on' if r['busCompEnabled'] else 'off'})",
             "  input dB   | matched: LUFS  crest  floor   slope | bypassed: LUFS  crest  floor   slope"]
    for i, o in enumerate(offs):
        m, b = r["matched"]["rows"][i], r["bypassed"]["rows"][i]
        sm = _f(r["matched"]["slopes"][i - 1], "{:+.2f}") if i else "    -"
        sb = _f(r["bypassed"]["slopes"][i - 1], "{:+.2f}") if i else "    -"
        lines.append(f"  {o:+6.1f}     | {_f(m['lufs'], '{:7.1f}')} {_f(m['crestDb'], '{:6.1f}')} {_f(m['floorDb'], '{:6.1f}')} {sm:>7}"
                     f" | {_f(b['lufs'], '{:7.1f}')} {_f(b['crestDb'], '{:6.1f}')} {_f(b['floorDb'], '{:6.1f}')} {sb:>7}")
    lines.append(f"  max |slope difference| matched vs bypassed: {_f(r['maxAbsSlopeDiff'], '{:.2f}')}")
    lv = r.get("live")
    if lv:                                   # third set: the live dynamics (what a rig plays), after matched vs bypassed
        lines.append("  live set (dynamicsMode live) | LUFS  crest  floor   slope")
        for i, o in enumerate(offs):
            m = lv["rows"][i]
            sl = _f(lv["slopes"][i - 1], "{:+.2f}") if i else "    -"
            lines.append(f"  {o:+6.1f}     | {_f(m['lufs'], '{:7.1f}')} {_f(m['crestDb'], '{:6.1f}')} {_f(m['floorDb'], '{:6.1f}')} {sl:>7}")
        lines.append(f"  max |slope difference| live vs bypassed: {_f(r.get('liveMaxAbsSlopeDiff'), '{:.2f}')}; "
                     f"live gate {'on' if r.get('liveGateEnabled') else 'off'}, live bus comp {'on' if r.get('liveBusCompEnabled') else 'off'}")
    return "\n".join(lines)



"""report.png: 1/3-octave LTAS vs. rule bands (and vs. reference)."""
from __future__ import annotations

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

from .rules import parse_expr  # noqa: E402

STATUS_COLOUR = {"pass": "#2e7d32", "marginal": "#ef6c00", "fail": "#c62828"}


def _group_span(bands: list[float]) -> tuple[float, float]:
    return bands[0] * 2 ** (-1 / 6), bands[-1] * 2 ** (1 / 6)


def allowed_intervals(groups_db: dict, rules: list[dict]) -> dict[str, tuple[float, float]]:
    """Per group, the [lo, hi] dB interval allowed by the rules given the other groups' actual levels."""
    iv: dict[str, list[float]] = {}
    for r in rules:
        e = parse_expr(r["expr"])
        thr = groups_db[e.rhs] + e.offset
        lo, hi = iv.setdefault(e.lhs, [-np.inf, np.inf])
        if e.op == "<=":
            iv[e.lhs][1] = min(hi, thr)
        else:
            iv[e.lhs][0] = max(lo, thr)
    return {k: (v[0], v[1]) for k, v in iv.items()}


def make_plot(path, title: str, centres, rel_db, groups_db, groups_def, results, ref=None) -> None:
    nrows = 2 if ref else 1
    fig, axes = plt.subplots(nrows, 1, figsize=(10, 4.6 * nrows), squeeze=False)
    ax = axes[0, 0]
    ivs = allowed_intervals(groups_db, _rules_from(results, groups_db))
    lo_plot = min(-60.0, float(np.min(rel_db)) - 3)
    for name, bands in groups_def.items():
        a, b = _group_span(bands)
        lo, hi = ivs.get(name, (-np.inf, np.inf))
        ax.axvspan(a, b, color="#9e9e9e", alpha=0.08)
        if name in ivs:
            ax.fill_between([a, b], max(lo, lo_plot), min(hi, 15), color="#2e7d32", alpha=0.22, lw=0)
        ax.hlines(groups_db[name], a, b, colors="k", lw=2)
        ax.text(np.sqrt(a * b), 13, name, ha="center", fontsize=8)
    ax.semilogx(centres, rel_db, "o-", color="#1565c0", label="output")
    if ref:
        ax.semilogx(centres, ref["rel_db"], "s--", color="#8e24aa", label="reference")
    ax.set_ylim(lo_plot, 15)
    ax.set_xlim(22, 15000)
    ax.set_ylabel("dB re 1 kHz band")
    ax.set_title(title)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(loc="lower left")
    fails = [r for r in results if r["status"] != "pass"]
    if fails:
        ax.text(0.99, 0.02, "\n".join(f"{r['status']}: {r['id']} ({r['margin']:+.1f} dB)" for r in fails),
                transform=ax.transAxes, ha="right", va="bottom", fontsize=7,
                color="#c62828", family="monospace")
    if ref:
        ax2 = axes[1, 0]
        d = ref["diff_db"]
        ax2.semilogx(centres, d, "o-", color="#e65100")
        ax2.axhline(0, color="k", lw=0.8)
        ax2.axvspan(80, 8000, color="#9e9e9e", alpha=0.1)
        ax2.set_xlim(22, 15000)
        ax2.set_ylabel("output - reference (dB)")
        ax2.set_xlabel("1/3-octave centre (Hz)")
        ax2.set_title(f"A-weighted error 80 Hz-8 kHz: {ref['aWeightedErrorDb']:.2f} dB RMS")
        ax2.grid(True, which="both", alpha=0.3)
    else:
        ax.set_xlabel("1/3-octave centre (Hz)")
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def _rules_from(results, groups_db):
    return [{"expr": r["expr"]} for r in results if r["group"] in groups_db]

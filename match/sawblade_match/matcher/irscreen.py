"""Analytic IR screen (v0.4M Task B3): rank thousands of IRs without rendering them.

After the NAM blocks everything is linear, and the cab is the last linear stage (post EQ aside). So for a refined candidate
the chain is rendered ONCE up to the cab (cab disabled, post EQ neutral): ``y_pre``. An IR then only multiplies the
spectrum: its predicted Welch PSD is ``PSD(y_pre) * |H|^2`` (|H| cached per IR on the loss' own Welch grid, see irlib.py;
the IR is the core's: left channel, 48 kHz, <= 2 s, L2 = 1). From that

* the LTAS error of the loss (A-weighted, same bands / hf limit / offset removal) is computed for every IR at once with one
  matrix product, and
* the spectral fizz sub-terms (``hfRatioDb`` from the frame periodograms x |H|^2, ``hfFlat``), W1 against the reference's
  distributions, Huber-softened and weighted exactly like the feel term (``hfMod`` is a time-domain envelope statistic and
  is left to the full render).

``score = ltas + W_FIZZ * scale * fizz``. Only the top N IRs get the real full-loss render (cabsweep.cab_sweep); the top 6
are exposed for the later two-IR pair search.

Above ``--ir-screen-max`` IRs the pool is prefiltered (never randomly sampled): IRs whose cab/speaker tags match the
candidate's current cab first, then a seeded k-means (k = 32) of the 1/3-octave |H| shape keeping, per cluster, the IRs
nearest the centroid in proportion to the cluster size.
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field

import numpy as np

from . import feel as F
from . import irlib
from . import loss as L

TOP_N = 24                 # IRs per candidate that get full renders
TOP_PAIR = 6               # IRs exposed for the pair search
SCREEN_MAX = 6000
KMEANS_K = 32
CHUNK = 1000
_FIZZ_KERNEL: np.ndarray | None = None


# ---- the bank --------------------------------------------------------------------------------------------------------
@dataclass
class Bank:
    caps: list                              # Capture per row
    h2: np.ndarray                          # (N, 4097) float32 |H|^2 on the Welch grid
    skipped: list = field(default_factory=list)
    n_local: int = 0
    n_pool: int = 0
    tags: list = field(default_factory=list)       # per row: tuple of cab/speaker tags
    _shape: np.ndarray | None = field(default=None, repr=False)
    _h2f: np.ndarray | None = field(default=None, repr=False)

    def __len__(self) -> int:
        return len(self.caps)

    @staticmethod
    def from_arrays(caps: list, h: np.ndarray, **kw) -> "Bank":
        h = np.asarray(h, np.float32)
        return Bank(list(caps), h * h, tags=[tuple(getattr(c, "tags", ())) for c in caps], **kw)

    @staticmethod
    def build(library, pool_cabs: list, cdir=None) -> "Bank":
        """Library IRs (their cached |H|) plus the pool's TONE3000 cabs (|H| from the same sidecar cache, rendered once)."""
        caps = list(library.captures()) if library is not None else []
        rows = [library.h] if library is not None and len(caps) else []
        n_local, skipped = len(caps), []
        extra, extra_caps = [], []
        for c in pool_cabs:
            v = irlib.load_sidecar(c.sha256, cdir)
            if v is None:
                try:
                    ir = irlib._render_ir(c.path)
                    h, w = irlib.response_from_ir(ir), irlib.window_from_ir(ir)
                    irlib.save_sidecar(c.sha256, h, w, cdir)
                    v = (h, w)
                except Exception as e:
                    skipped.append(f"{c.key}: {type(e).__name__}: {e}")
                    continue
            extra.append(v[0])
            extra_caps.append(c)
        if extra:
            rows.append(np.stack(extra))
        h = np.concatenate(rows) if rows else np.zeros((0, irlib.H_BINS), np.float32)
        b = Bank.from_arrays(caps + extra_caps, h, skipped=skipped, n_local=n_local, n_pool=len(extra_caps))
        b.tags = [tuple(irlib_tags(c)) for c in b.caps]
        return b

    def shape_db(self) -> np.ndarray:
        """(N, 27) 1/3-octave |H|^2 in dB, mean removed (a level-free spectral family descriptor)."""
        if self._shape is None:
            p = self.h2 @ L.BAND_W.T.astype(np.float32)
            d = 10 * np.log10(np.maximum(p, 1e-30))
            self._shape = (d - d.mean(axis=1, keepdims=True)).astype(np.float32)
        return self._shape

    def h2_fizz(self) -> np.ndarray:
        """|H|^2 on the 2048-pt grid of the fizz frames (smoothed by that Hann window's power kernel), (N, 1025)."""
        global _FIZZ_KERNEL
        if self._h2f is None:
            if _FIZZ_KERNEL is None:
                k = np.abs(np.fft.rfft(F._WIN_FIZZ, irlib.NFFT)[:17]) ** 2
                k = np.concatenate([k[:0:-1], k])
                _FIZZ_KERNEL = (k / k.sum()).astype(np.float32)
            k = _FIZZ_KERNEL
            hp = np.pad(self.h2, ((0, 0), (16, 16)), mode="edge")
            out = np.zeros((len(self.h2), F.FIZZ_N // 2 + 1), np.float32)
            for t, kt in enumerate(k):
                out += kt * hp[:, t: t + 4 * (F.FIZZ_N // 2) + 1: 4]
            self._h2f = out
        return self._h2f


def irlib_tags(cap) -> list[str]:
    """cab/speaker tags of a capture: stored for local IRs, parsed from the title/name for TONE3000 cabs."""
    if getattr(cap, "provider", "tone3000") == "local":
        return list(cap.tags)
    t = irlib.tags_for(f"{cap.title} {cap.name}")
    return t["cab"] + t["mic"] + t["position"]


# ---- the screen ------------------------------------------------------------------------------------------------------
def pre_cab(eng, cand, ex) -> np.ndarray:
    """The candidate's output up to the cab on the excerpt: path EQ / level / blend / alignment, cab disabled, post EQ neutral."""
    v = cand.extra["params"]
    paths = ("a", "b") if cand.combo.topology == "blend" else ("a",)
    outs = []
    for p in paths:
        core = eng.core(cand.combo, v, p, ex.x)
        pre = eng.linear_preset(cand.combo.cab, v, p)
        pre["cab"]["enabled"] = False
        pre["postEq"] = []
        y, _ = eng.render(pre, core)
        outs.append(y)
    y = outs[0] if len(outs) == 1 else eng.mix(outs[0], outs[1], v["blend"], cand.align, cand.levels)
    return ex.trim(y)


def ltas_batch(pred_db: np.ndarray, ref_db: np.ndarray, hf_limit_hz: float | None) -> tuple[np.ndarray, np.ndarray]:
    """``loss.ltas_error`` for every row of ``pred_db`` (N, bands): (error dB, offset dB)."""
    d = pred_db - np.asarray(ref_db)[None, :]
    w = L.A_POWER_W
    if hf_limit_hz is None:
        off = (d * w).sum(axis=1) / w.sum()
        d = d - off[:, None]
        return np.sqrt((w * d * d).sum(axis=1) / w.sum()), off
    keep = L.BAND_UPPER <= hf_limit_hz
    if not keep.any():
        keep = np.ones_like(keep)
    off = (d[:, keep] * w[keep]).sum(axis=1) / w[keep].sum()
    d = d - off[:, None]
    d = np.where(keep[None, :], d, np.maximum(d, 0.0))
    return np.sqrt((w * d * d).sum(axis=1) / w.sum()), off


def _huber(x: np.ndarray) -> np.ndarray:
    x = np.abs(x)
    return np.where(x < F.SOFT_DELTA, x * x / (2.0 * F.SOFT_DELTA), x - 0.5 * F.SOFT_DELTA)


def predicted_fizz(bank: Bank, y: np.ndarray, ft: "F.FeelTarget") -> np.ndarray | None:
    """Weighted spectral fizz sub-terms (hfRatioDb, hfFlat) of every IR, from the frame periodograms of ``y`` x |H|^2.
    None when the feel target has no usable fizz (the term is off)."""
    if ft is None or not ft.fizz_on or ft.ref.fizz is None:
        return None
    starts = np.asarray(ft.plan.fizz_starts)
    if len(starts) < F.MIN_FRAMES:
        return None
    y = np.asarray(y, np.float64)
    if len(y) < ft.plan.n:
        y = np.concatenate([y, np.zeros(ft.plan.n - len(y))])
    idx = starts[:, None] + np.arange(F.FIZZ_N)[None, :]
    P = np.abs(np.fft.rfft(y[idx] * F._WIN_FIZZ[None, :], axis=1)) ** 2
    P = (P / max(P.max(), 1e-300)).astype(np.float32)
    H = bank.h2_fizz()
    sh, sm, sf = F._SEL_HF, F._SEL_MID, F._SEL_FLAT
    lp = np.log(P[:, sf] + 1e-30).mean(axis=1)                       # (F,)
    n_flat = int(sf.sum())
    ref_r, ref_f = ft.ref.fizz["hfRatioDb"], ft.ref.fizz["hfFlat"]
    qr, qf = np.quantile(ref_r, F.Q_LEVELS), np.quantile(ref_f, F.Q_LEVELS)
    out = np.zeros(len(bank))
    for s in range(0, len(bank), CHUNK):
        h = H[s: s + CHUNK]
        hf = P[:, sh] @ h[:, sh].T
        mid = P[:, sm] @ h[:, sm].T
        ratio = 10.0 * np.log10((hf + 1e-30) / (mid + 1e-30))
        am = (P[:, sf] @ h[:, sf].T) / n_flat
        lh = np.log(h[:, sf] + 1e-30).mean(axis=1)
        flat = np.exp(lp[:, None] + lh[None, :]) / np.maximum(am, 1e-30)
        a = np.abs(np.quantile(ratio, F.Q_LEVELS, axis=0) - qr[:, None]).mean(axis=0)
        b = np.abs(np.quantile(flat, F.Q_LEVELS, axis=0) - qf[:, None]).mean(axis=0)
        out[s: s + CHUNK] = _huber(a / F.RATIO_NORM_DB) + _huber(b / F.FLAT_NORM)
    return F.W_FIZZ * ft.scale * out


@dataclass
class Screened:
    score: np.ndarray           # ltas + weighted fizz (lower is better)
    ltas: np.ndarray
    fizz: np.ndarray            # weighted spectral fizz (zeros when the fizz term is off)
    offset: np.ndarray
    fizz_on: bool
    seconds: float

    def order(self) -> np.ndarray:
        return np.argsort(self.score, kind="stable")


def screen(bank: Bank, y_pre: np.ndarray, tgt: "L.Target", rows: np.ndarray | None = None) -> Screened:
    """Analytic score of every IR (or of ``rows``) for the pre-cab signal ``y_pre`` against the target."""
    t0 = time.time()
    h2 = bank.h2 if rows is None else bank.h2[rows]
    sub = bank if rows is None else Bank(bank.caps, h2, _h2f=bank.h2_fizz()[rows])
    y = np.asarray(y_pre, np.float64)
    X = L.segment_spectra(y, tgt.starts)
    psd = L.psd_from_spectra(X)
    psd = psd / max(psd.max(), 1e-300)
    A = (L.BAND_W * psd[None, :]).T.astype(np.float32)               # (4097, bands)
    pw = h2 @ A
    pred = 10.0 * np.log10(np.maximum(pw.astype(np.float64), 1e-30))
    ltas, off = ltas_batch(pred, tgt.ref.band_db, tgt.hf_limit_hz)
    fz = predicted_fizz(sub, y, tgt.feel)
    fizz = np.zeros(len(h2)) if fz is None else fz
    return Screened(ltas + fizz, ltas, fizz, off, fz is not None, time.time() - t0)


def rank_correlation(a: np.ndarray, b: np.ndarray) -> float:
    """Spearman rank correlation."""
    ra = np.argsort(np.argsort(a, kind="stable"), kind="stable").astype(float)
    rb = np.argsort(np.argsort(b, kind="stable"), kind="stable").astype(float)
    ra -= ra.mean()
    rb -= rb.mean()
    return float((ra * rb).sum() / max(np.sqrt((ra * ra).sum() * (rb * rb).sum()), 1e-30))


# ---- prefilter -------------------------------------------------------------------------------------------------------
def kmeans(x: np.ndarray, k: int, seed: int, iters: int = 30) -> tuple[np.ndarray, np.ndarray]:
    """Seeded Lloyd k-means (k-means++ start). Returns (labels, centres); deterministic for a given seed."""
    rng = np.random.default_rng(seed)
    x = np.asarray(x, np.float64)
    n = len(x)
    k = min(k, n)
    c = [x[int(rng.integers(n))]]
    d2 = ((x - c[0]) ** 2).sum(axis=1)
    for _ in range(1, k):
        p = d2 / d2.sum() if d2.sum() > 0 else np.full(n, 1.0 / n)
        c.append(x[int(rng.choice(n, p=p))])
        d2 = np.minimum(d2, ((x - c[-1]) ** 2).sum(axis=1))
    c = np.array(c)
    lab = np.zeros(n, int)
    for it in range(iters):
        d = (x ** 2).sum(1)[:, None] - 2 * x @ c.T + (c ** 2).sum(1)[None, :]
        new = d.argmin(axis=1)
        if it and np.array_equal(new, lab):
            break
        lab = new
        for j in range(k):
            m = lab == j
            if m.any():
                c[j] = x[m].mean(axis=0)
    return lab, c


def kmeans_select(x: np.ndarray, n: int, k: int = KMEANS_K, seed: int = 0) -> np.ndarray:
    """``n`` row indices of ``x``: per k-means cluster the rows nearest its centre, quota proportional to the cluster size
    (largest remainder). Never random: ties break by index."""
    N = len(x)
    if N <= n:
        return np.arange(N)
    lab, c = kmeans(x, k, seed)
    kk = len(c)
    sizes = np.array([(lab == j).sum() for j in range(kk)])
    raw = n * sizes / N
    q = np.minimum(np.floor(raw).astype(int), sizes)
    rem = n - int(q.sum())
    for j in np.argsort(-(raw - np.floor(raw)), kind="stable"):
        if rem <= 0:
            break
        if q[j] < sizes[j]:
            q[j] += 1
            rem -= 1
    j = 0
    while rem > 0:                        # (only if every fractional slot was full)
        if q[j % kk] < sizes[j % kk]:
            q[j % kk] += 1
            rem -= 1
        j += 1
    sel = []
    for j in range(kk):
        idx = np.nonzero(lab == j)[0]
        d = ((x[idx] - c[j]) ** 2).sum(axis=1)
        sel += list(idx[np.argsort(d, kind="stable")[: q[j]]])
    return np.array(sorted(sel))


def prefilter(bank: Bank, max_n: int, hints: set[str], seed: int = 0) -> tuple[np.ndarray, dict]:
    """Row indices kept when the bank is larger than ``max_n`` (all rows otherwise), and a record of what was done."""
    N = len(bank)
    info = {"prefiltered": False, "method": None, "before": N, "after": N, "tagHints": sorted(hints), "tagKept": 0,
            "clusters": 0}
    if N <= max_n:
        return np.arange(N), info
    shape = bank.shape_db()
    tagged = np.array([i for i, t in enumerate(bank.tags) if hints & set(t)], int) if hints else np.zeros(0, int)
    keep = np.zeros(0, int)
    if len(tagged):
        keep = tagged if len(tagged) <= max_n // 2 else tagged[kmeans_select(shape[tagged], max_n // 2, KMEANS_K, seed)]
    rest = np.setdiff1d(np.arange(N), keep)
    fill = max_n - len(keep)
    sel_rest = rest[kmeans_select(shape[rest], fill, KMEANS_K, seed)]
    out = np.sort(np.concatenate([keep, sel_rest]))
    info.update(prefiltered=True, method=("tags+kmeans" if len(keep) else "kmeans") + f"{KMEANS_K}", after=len(out),
                tagKept=int(len(keep)), clusters=KMEANS_K)
    return out, info


# ---- per-candidate entry point ---------------------------------------------------------------------------------------
def candidate_irs(bank: Bank, eng, cand, ex, tgt, *, top_n: int = TOP_N, screen_max: int = SCREEN_MAX, seed: int = 0) -> tuple[list, dict]:
    """Screen the whole bank for ``cand`` and return (captures for the full sweep, record). The candidate's own cab is always
    among the captures (its row is the reference of the sweep). ``record['top']`` is the analytic ranking of the top N,
    ``record['top6']`` the analytic top 6 (the pair-search input)."""
    t0 = time.time()
    hints = set(irlib_tags(cand.combo.cab)) & set(irlib.TAG_PATTERNS["cab"])
    rows, pre = prefilter(bank, screen_max, hints, seed)
    y = pre_cab(eng, cand, ex)
    s = screen(bank, y, tgt, rows if pre["prefiltered"] else None)
    order = s.order()[:top_n]
    glob = rows[order] if pre["prefiltered"] else order
    caps = [bank.caps[i] for i in glob]
    top = [{"cab": bank.caps[g].key, "title": bank.caps[g].title, "score": float(s.score[o]), "ltas": float(s.ltas[o]),
            "fizz": float(s.fizz[o])} for o, g in zip(order, glob)]
    cur = cand.combo.cab
    if all(c.key != cur.key for c in caps):
        caps.append(cur)
    rec = {"topology": cand.topology, "boost": bool(cand.combo.boost), "pool": len(bank), "screened": int(len(s.score)),
           "fizzTerm": s.fizz_on, "prefilter": pre, "top": top, "top6": [t["cab"] for t in top[:TOP_PAIR]],
           "screenSeconds": round(s.seconds, 3), "seconds": round(time.time() - t0, 3)}
    return caps, rec

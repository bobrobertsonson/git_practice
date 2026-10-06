"""IR library (v0.4M Task B3): the user's own IR catalog, indexed once and screened analytically.

``scan(dirs)`` walks the directories (recursively, spaces / unicode / symlink loops are fine), decodes every audio file
with soundfile, never fails on a single file (it is rejected with a reason), and caches an index at
``~/.cache/sawblade/ir_index.json`` keyed by (path, size, mtime_ns), so an unchanged file is not read again. The index is
saved at least every 2 s while a cold scan runs, so an interrupted scan keeps what it indexed and resumes.

The IR as the renderer sees it (left channel, resampled to 48 kHz, truncated at 2 s, L2 = 1) is obtained by rendering a
unit impulse through the C++ core with the file as the cab (no DSP is reimplemented here); from it a sidecar
``<cache>/ir_h_v1/<sha[:2]>/<sha>.npy`` holds |H| on the Welch grid (4097 bins of the 8192-pt Hann Welch estimate of the
loss, power smoothed by the Hann window's power kernel, i.e. the expected Welch response) plus a 50 ms peak-aligned window
used for near-duplicate detection. Warm runs read the sidecars only (no FFT, no render).

Licence: local IRs are ``user-owned``; never committed, never uploaded. Presets reference them by absolute path + sha256.
Formats the core cannot read (.aif/.aiff/.flac or a WAV subtype it rejects) are converted to a float32 left-channel WAV in
``<cache>/ir_wav/`` (no resampling); that file is what the preset points at.

    python -m sawblade_match.matcher.irlib --scan DIR [--scan DIR ...] [--json OUT]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Sequence

import numpy as np
import soundfile as sf

from ..tonecheck.analysis import NFFT

RATE = 48000
MAX_IR_S = 2.0
MIN_IR_S = 0.002
MAX_IR_SAMPLES = int(MAX_IR_S * RATE)
H_BINS = NFFT // 2 + 1
FINE = 16                         # |H| is evaluated on a 16x finer grid, then smoothed to the Welch grid
PRE = 24                          # near-duplicate window starts this many samples before the peak
WIN_N = PRE + 2400                # ... and covers 50 ms after it
NEAR_CORR = 0.999
INDEX_VERSION = 1
SIDECAR_DIR = "ir_h_v1"
AUDIO_EXT = (".wav", ".aif", ".aiff", ".flac")
SAVE_EVERY_S = 2.0
PROGRESS_EVERY_S = 1.0
EXAMPLES = 20


# ---- locations -------------------------------------------------------------------------------------------------------
def cache_dir() -> Path:
    return Path.home() / ".cache" / "sawblade"


def index_path() -> Path:
    return cache_dir() / "ir_index.json"


def config_path() -> Path:
    return Path.home() / ".config" / "sawblade" / "ir_dirs.json"


def load_dirs() -> list[str]:
    try:
        d = json.loads(config_path().read_text())
        return [str(x) for x in d.get("dirs", [])]
    except (OSError, ValueError, AttributeError):
        return []


def add_dir(d: str) -> list[str]:
    """Add ``d`` (made absolute) to the persistent list; returns the list."""
    dirs = load_dirs()
    d = str(Path(d).expanduser().resolve())
    if d not in dirs:
        dirs.append(d)
    p = config_path()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({"dirs": dirs}, indent=2) + "\n")
    return dirs


# ---- tags ------------------------------------------------------------------------------------------------------------
_L, _D = r"(?<![a-z0-9])", r"(?!\d)"
TAG_PATTERNS: dict[str, dict[str, str]] = {
    "cab": {       # cab models, brands and speakers
        "v30": _L + r"v-?30" + _D, "g12t75": _L + r"g-?12-?t-?75", "greenback": r"greenback|" + _L + r"g-?12-?m" + _D + r"|" + _L + r"gb(?![a-z])",
        "creamback": r"creamback|g12h-?30", "c90": _L + r"c-?90" + _D, "k100": _L + r"(?:g12-?)?k-?100" + _D,
        "g12-65": r"g-?12-?65", "ev12l": r"ev-?12l", "jensen": "jensen",
        "1960": _L + r"1960[a-z]?" + _D, "4x12": _L + r"4\s?x\s?12" + _D + r"|" + _L + r"412" + _D,
        "2x12": _L + r"2\s?x\s?12" + _D, "1x12": _L + r"1\s?x\s?12" + _D,
        "mesa": r"mesa|rectifier|(?<![a-z])recto|dual\s?rec", "marshall": "marshall", "orange": "orange",
        "engl": r"(?<![a-z])engl", "peavey": r"peavey|5150", "bogner": "bogner", "diezel": "diezel", "soldano": "soldano",
        "fender": "fender", "vox": r"(?<![a-z])vox", "ampeg": "ampeg",
        "os": _L + r"os(?![a-z0-9])|oversized", "standard": r"(?<![a-z])(?:standard|std)(?![a-z])",
    },
    "mic": {
        "sm57": _L + r"sm-?57" + _D, "sm58": _L + r"sm-?58" + _D, "md421": _L + r"md-?421", "r121": _L + r"r-?121",
        "414": _L + r"(?:c-?)?414" + _D, "u87": _L + r"u-?87", "e609": _L + r"e-?609", "e906": _L + r"e-?906",
        "ribbon": "ribbon|royer", "m160": _L + r"m-?160", "beta57": r"beta-?57", "sm7": _L + r"sm-?7(?![0-9])",
    },
    "position": {
        "capedge": r"cap[- ]?edge", "cap": r"(?<![a-z])cap(?![a-z])", "edge": r"(?<![a-z])edge(?![a-z])",
        "cone": r"(?<![a-z])cone", "offaxis": r"off-?axis", "onaxis": r"on-?axis", "center": r"cent(?:er|re)",
        "room": r"(?<![a-z])room(?![a-z])", "close": r"(?<![a-z])close(?![a-z])",
    },
}
_COMPILED = {g: {k: re.compile(p) for k, p in d.items()} for g, d in TAG_PATTERNS.items()}
_DIST = re.compile(r"(\d+(?:\.\d+)?)\s?(?:in(?:ch(?:es)?)?(?![a-z])|\")")


def tags_for(text: str) -> dict[str, list[str]]:
    """Tags of a path string (folders + file name), case-insensitive. Groups: cab (cab/speaker), mic, position."""
    s = text.lower().replace("_", " ")
    out = {g: [k for k, rx in d.items() if rx.search(s)] for g, d in _COMPILED.items()}
    out["position"] += [f"dist:{m.group(1)}in" for m in _DIST.finditer(s)][:1]
    return out


# ---- analysis of one IR ----------------------------------------------------------------------------------------------
_KERNEL: np.ndarray | None = None


def _kernel() -> np.ndarray:
    """Power kernel of the periodic Hann window on the fine grid (the expected Welch smoothing of |H|^2), sum 1."""
    global _KERNEL
    if _KERNEL is None:
        w = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(NFFT) / NFFT)
        k = np.abs(np.fft.rfft(w, NFFT * FINE)[: 4 * FINE + 1]) ** 2
        k = np.concatenate([k[:0:-1], k])
        _KERNEL = k / k.sum()
    return _KERNEL


def response_from_ir(ir: np.ndarray) -> np.ndarray:
    """|H| (float32, ``H_BINS`` bins of the Welch grid) of an IR at 48 kHz, L2 = 1 and truncated like the core's."""
    n = NFFT * FINE
    p = np.abs(np.fft.rfft(np.asarray(ir, np.float64)[:MAX_IR_SAMPLES], n)) ** 2
    k = _kernel()
    h = len(k) // 2
    pp = np.pad(p, h, mode="reflect")
    m = 1 << int(np.ceil(np.log2(len(pp) + len(k))))
    sm = np.fft.irfft(np.fft.rfft(pp, m) * np.fft.rfft(k, m), m)[2 * h: 2 * h + len(p)]
    return np.sqrt(np.maximum(sm[::FINE][:H_BINS], 0.0)).astype(np.float32)


def window_from_ir(ir: np.ndarray) -> np.ndarray:
    """50 ms after (and 0.5 ms before) the peak, float32 ``WIN_N`` samples (the near-duplicate signature)."""
    ir = np.asarray(ir, np.float32)
    p = int(np.argmax(np.abs(ir)))
    a = p - PRE
    w = np.zeros(WIN_N, np.float32)
    lo = max(a, 0)
    seg = ir[lo: a + WIN_N]
    w[lo - a: lo - a + len(seg)] = seg
    return w


def _render_ir(path: str) -> np.ndarray:
    """The IR exactly as the core applies it: render a unit impulse with the file as the (only) cab."""
    from ..core import render           # lazy: --help and tag tests do not need the extension
    preset = {"schema": "sawblade.preset", "version": 1, "name": "ir", "gate": {"enabled": False},
              "paths": {"a": {"role": "saw", "blocks": []}, "b": {"role": "body", "enabled": False, "blocks": []}},
              "align": {"mode": "off"}, "blend": 0.0, "cab": {"mode": "shared", "ir": {"file": str(path)}, "enabled": True},
              "postEq": [], "output": {"gainDb": 0.0}}
    imp = np.zeros(MAX_IR_SAMPLES, np.float32)
    imp[0] = 1.0
    y, _ = render(preset, imp, float(RATE))
    return np.asarray(y, np.float32)


def file_sha256(path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def sidecar_path(sha: str, cdir: Path | None = None) -> Path:
    return (cdir or cache_dir()) / SIDECAR_DIR / sha[:2] / f"{sha}.npy"


def save_sidecar(sha: str, h: np.ndarray, w: np.ndarray, cdir: Path | None = None) -> None:
    p = sidecar_path(sha, cdir)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + f".{os.getpid()}.tmp")
    with open(tmp, "wb") as f:
        np.save(f, np.concatenate([h, w]).astype(np.float32))
    os.replace(tmp, p)


def load_sidecar(sha: str, cdir: Path | None = None) -> tuple[np.ndarray, np.ndarray] | None:
    try:
        v = np.load(sidecar_path(sha, cdir))
    except (OSError, ValueError):
        return None
    if v.shape != (H_BINS + WIN_N,):
        return None
    return v[:H_BINS], v[H_BINS:]


def _reject(reason: str, detail: str = "") -> dict:
    return {"st": "rej", "reason": reason, "detail": detail[:200]}


def analyze_file(path: str, cdir: Path | None = None) -> dict:
    """Index entry of one file (never raises): ``st`` "ok" with the metadata, or "rej" with ``reason`` / ``detail``.
    Writes the sidecar of an accepted IR."""
    p = Path(path)
    name = p.name
    if name.startswith("._"):
        return _reject("resource-fork", "macOS AppleDouble file")
    try:
        sha = file_sha256(p)
        info = sf.info(str(p)) if hasattr(sf, "info") else None
        want = None
        if info is not None:
            want = int(MAX_IR_S * info.samplerate) + 16
        x, fs = sf.read(str(p), dtype="float32", always_2d=True, **({"frames": want} if want else {}))
        fs = int(fs)
        frames = int(info.frames) if info is not None else int(x.shape[0])
        channels = int(x.shape[1])
    except Exception as e:      # corrupt, truncated, unsupported: never fatal
        return _reject("unreadable", f"{type(e).__name__}: {e}")
    if frames == 0 or x.shape[0] == 0:
        return _reject("empty", "no samples")
    left = np.ascontiguousarray(x[:, 0])
    if frames / fs < MIN_IR_S:
        return _reject("too-short", f"{1000.0 * frames / fs:.2f} ms")
    if not np.all(np.isfinite(left)):
        return _reject("unreadable", "non-finite samples")
    if float(np.max(np.abs(left))) < 1e-7:
        return _reject("silent", "all samples are (near) zero")
    ent = {"st": "ok", "sha": sha, "rate": fs, "ch": channels, "frames": frames, "trunc": bool(frames / fs > MAX_IR_S + 1e-9),
           "origS": round(frames / fs, 6), "core": str(p), "coreSha": sha}
    core_path = str(p)
    ir = None
    if p.suffix.lower() == ".wav":
        try:
            ir = _render_ir(core_path)
        except ImportError:
            raise
        except Exception:        # the core could not read this WAV subtype: convert
            ir = None
    if ir is None:
        conv = (cdir or cache_dir()) / "ir_wav" / f"{sha}.wav"
        try:
            conv.parent.mkdir(parents=True, exist_ok=True)
            sf.write(str(conv), left[: int(MAX_IR_S * fs) + 16], fs, subtype="FLOAT")
            ir = _render_ir(str(conv))
        except ImportError:
            raise
        except Exception as e:
            return _reject("unreadable", f"core cannot load it: {type(e).__name__}: {e}")
        ent["core"], ent["coreSha"] = str(conv), file_sha256(conv)
    if not np.any(np.abs(ir) > 0) or not np.all(np.isfinite(ir)):
        return _reject("silent", "the core rendered no response")
    save_sidecar(sha, response_from_ir(ir), window_from_ir(ir), cdir)
    return ent


# ---- records / library -----------------------------------------------------------------------------------------------
@dataclass
class IrRecord:
    path: str
    root: str
    sha256: str
    rate: int
    channels: int
    frames: int
    truncated: bool
    orig_seconds: float
    tags: dict
    core_path: str
    core_sha: str
    size: int
    aliases: list = field(default_factory=list)       # [{"path", "reason"}]

    @property
    def stem(self) -> str:
        return Path(self.path).stem

    def capture(self):
        """Pool ``Capture`` for the matcher: provider "local", licence "user-owned"."""
        from .pool import Capture
        flat = " ".join(v for g in self.tags.values() for v in g)
        return Capture(0, 0, self.stem, flat, "cab", self.core_path, self.core_sha, self.size, "user-owned", "", "", "cab",
                       provider="local", local_id=self.sha256[:16], orig_path=self.path, orig_sha=self.sha256,
                       tags=tuple(sorted({v for g in self.tags.values() for v in g})))


@dataclass
class IrLibrary:
    records: list[IrRecord]
    report: dict
    h: np.ndarray | None = None             # (N, H_BINS) float32 |H|, rows aligned with ``records``

    def captures(self) -> list:
        return [r.capture() for r in self.records]


def _walk(root: Path, errors: list) -> Iterable[Path]:
    """All files under ``root``, sorted; follows symlinks but visits each real directory once."""
    seen: set[str] = set()
    stack = [root]
    while stack:
        d = stack.pop()
        try:
            rp = os.path.realpath(d)
            if rp in seen:
                continue
            seen.add(rp)
            entries = sorted(os.scandir(d), key=lambda e: e.name)
        except OSError as e:
            errors.append(f"{d}: {e}")
            continue
        subs = []
        for e in entries:
            try:
                if e.is_dir(follow_symlinks=True):
                    subs.append(Path(e.path))
                elif e.is_file(follow_symlinks=True):
                    yield Path(e.path)
            except OSError as ex:
                errors.append(f"{e.path}: {ex}")
        stack.extend(reversed(subs))


def _load_index(ip: Path) -> dict:
    try:
        d = json.loads(ip.read_text())
        if d.get("version") == INDEX_VERSION:
            return d.get("files", {})
    except (OSError, ValueError, AttributeError):
        pass
    return {}


def _save_index(ip: Path, files: dict) -> None:
    ip.parent.mkdir(parents=True, exist_ok=True)
    tmp = ip.with_name(ip.name + f".{os.getpid()}.tmp")
    tmp.write_text(json.dumps({"version": INDEX_VERSION, "files": files}))
    os.replace(tmp, ip)


def scan(dirs: Sequence[str | Path], *, index_file: Path | None = None, cdir: Path | None = None, workers: int = 4,
         progress: Callable[[int, int, float], None] | None = None, analyze=None, dedupe: bool = True) -> IrLibrary:
    """Index ``dirs`` and return the library (deduplicated, accepted IRs) with the scan report. ``progress(done, total,
    eta_s)`` is called at least every 2 s during the cold part. ``analyze`` replaces ``analyze_file`` (tests)."""
    analyze = analyze or analyze_file
    ip = index_file or index_path()
    index = _load_index(ip)
    errors: list[str] = []
    files: list[tuple[Path, Path]] = []                    # (file, root)
    seen_paths: set[str] = set()
    n_seen = 0
    not_audio: Counter = Counter()
    for d in dirs:
        root = Path(d).expanduser()
        if not root.is_dir():
            errors.append(f"{root}: not a directory")
            continue
        for f in _walk(root, errors):
            if str(f) in seen_paths:         # the same file reached twice (overlapping roots)
                continue
            seen_paths.add(str(f))
            n_seen += 1
            if f.suffix.lower() not in AUDIO_EXT:
                not_audio[f.suffix.lower() or "(none)"] += 1
                continue
            files.append((f, root))
    todo: list[Path] = []
    stat: dict[str, tuple[int, int]] = {}
    for f, _ in files:
        try:
            s = f.stat()
        except OSError as e:
            errors.append(f"{f}: {e}")
            continue
        stat[str(f)] = (s.st_size, s.st_mtime_ns)
        e = index.get(str(f))
        if e and e.get("s") == s.st_size and e.get("m") == s.st_mtime_ns and \
                (e.get("st") != "ok" or load_sidecar(e["sha"], cdir) is not None):
            continue
        todo.append(f)
    reused = len(files) - len(todo)
    t0 = time.monotonic()
    last_p = last_s = t0
    done = 0
    ex = ThreadPoolExecutor(max(1, workers))
    pending = {}
    try:
        for f in todo:
            pending[ex.submit(analyze, str(f), cdir)] = f
        while pending:
            fin, _ = wait(list(pending), timeout=0.5, return_when=FIRST_COMPLETED)
            for fu in fin:
                f = pending.pop(fu)
                try:
                    ent = fu.result()
                except ImportError:
                    raise
                except Exception as e:
                    ent = _reject("unreadable", f"{type(e).__name__}: {e}")
                ent["s"], ent["m"] = stat[str(f)]
                index[str(f)] = ent
                done += 1
            now = time.monotonic()
            if now - last_s >= SAVE_EVERY_S:
                _save_index(ip, index)
                last_s = now
            if progress and now - last_p >= PROGRESS_EVERY_S:
                progress(done, len(todo), (now - t0) / max(done, 1) * (len(todo) - done))
                last_p = now
        if todo and progress:
            progress(done, len(todo), 0.0)
    finally:
        ex.shutdown(wait=False, cancel_futures=True)
        if todo:
            _save_index(ip, index)
    return _build(files, index, not_audio, n_seen, errors, reused, cdir, dedupe, [str(d) for d in dirs], time.monotonic() - t0)


def _build(files, index, not_audio, n_seen, errors, reused, cdir, dedupe, dirs, seconds) -> IrLibrary:
    rejected: dict[str, list] = {}
    ok: list[IrRecord] = []
    for f, root in sorted(files, key=lambda t: str(t[0])):
        e = index.get(str(f))
        if e is None:
            continue
        if e["st"] != "ok":
            rejected.setdefault(e["reason"], []).append({"path": str(f), "detail": e.get("detail", "")})
            continue
        try:
            rel = str(f.relative_to(root))
        except ValueError:
            rel = f.name
        ok.append(IrRecord(str(f), str(root), e["sha"], e["rate"], e["ch"], e["frames"], e["trunc"], e["origS"],
                           tags_for(rel), e["core"], e["coreSha"], e["s"]))
    # exact duplicates: the first path (sorted) of a sha wins
    by_sha: dict[str, list[IrRecord]] = {}
    for r in ok:
        by_sha.setdefault(r.sha256, []).append(r)
    uniq: list[IrRecord] = []
    n_exact = 0
    for grp in by_sha.values():
        keep = grp[0]
        for o in grp[1:]:
            keep.aliases.append({"path": o.path, "reason": f"exact duplicate (same sha256) of {keep.path}"})
            n_exact += 1
        uniq.append(keep)
    uniq.sort(key=lambda r: r.path)
    vecs = [load_sidecar(r.sha256, cdir) for r in uniq]
    keep_idx = list(range(len(uniq)))
    near_pairs = []
    if dedupe and len(uniq) > 1:
        keep_idx, near_pairs = _near_dedupe(uniq, [v[1] for v in vecs])
    kept = [uniq[i] for i in keep_idx]
    h = np.stack([vecs[i][0] for i in keep_idx]) if kept else np.zeros((0, H_BINS), np.float32)
    n_near = len(uniq) - len(kept)
    rep = _report(ok, kept, rejected, not_audio, n_seen, n_exact, n_near, near_pairs, errors, reused, dirs, seconds)
    return IrLibrary(kept, rep, h)


# ---- near duplicates --------------------------------------------------------------------------------------------------
_EDGES = np.concatenate([[0.0], np.geomspace(60.0, 16000.0, 50), [24000.0]])


def _band_features(wins: np.ndarray) -> np.ndarray:
    """Unit-norm vectors of sqrt band energies (51 bands) of the 50 ms windows. Necessary condition of a waveform
    correlation c: |u - v|^2 <= 2 (1 - c) (magnitudes only, so sub-sample shifts do not matter)."""
    nf = 4096
    P = np.abs(np.fft.rfft(wins, nf, axis=1)) ** 2
    f = np.fft.rfftfreq(nf, 1.0 / RATE)
    idx = np.clip(np.searchsorted(_EDGES, f, side="right") - 1, 0, len(_EDGES) - 2)
    M = np.zeros((len(f), len(_EDGES) - 1))
    M[np.arange(len(f)), idx] = 1.0
    E = np.sqrt(P @ M)
    return (E / np.maximum(np.linalg.norm(E, axis=1, keepdims=True), 1e-30)).astype(np.float32)


def _xcorr_max(a: np.ndarray, b: np.ndarray, lag: int = 8, up: int = 16) -> float:
    """Max normalised cross-correlation over |lag| <= ``lag`` samples (fractional, band-limited interpolation)."""
    na, nb = np.linalg.norm(a), np.linalg.norm(b)
    if na == 0 or nb == 0:
        return 0.0
    nf = 8192
    X = np.fft.rfft(a, nf) * np.conj(np.fft.rfft(b, nf))
    c = np.fft.irfft(X, nf * up) * up
    L = lag * up
    return float(max(c[: L + 1].max(), c[-L:].max()) / (na * nb))


def _near_dedupe(uniq: list[IrRecord], wins: list[np.ndarray]) -> tuple[list[int], list[tuple]]:
    W = np.stack(wins).astype(np.float64)
    F = _band_features(W)
    n = len(uniq)
    parent = list(range(n))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    thr_cos = 1.0 - 0.5 * (2.0 * (1.0 - NEAR_CORR) * 4.0)      # generous x4 margin on the squared distance bound
    corr_of: dict[tuple[int, int], float] = {}
    for s in range(0, n, 512):
        S = F[s: s + 512] @ F.T
        for a in range(S.shape[0]):
            i = s + a
            for j in np.nonzero(S[a, i + 1:] >= thr_cos)[0] + i + 1:
                c = _xcorr_max(W[i], W[j])
                if c >= NEAR_CORR:
                    corr_of[(i, int(j))] = c
                    parent[find(int(j))] = find(i)
    groups: dict[int, list[int]] = {}
    for i in range(n):
        groups.setdefault(find(i), []).append(i)
    keep, pairs = [], []
    for g in groups.values():
        if len(g) == 1:
            keep.append(g[0])
            continue
        best = min(g, key=lambda i: (uniq[i].rate != RATE, -uniq[i].rate, uniq[i].path))
        keep.append(best)
        for i in g:
            if i == best:
                continue
            c = corr_of.get((min(i, best), max(i, best)))
            if c is None:
                c = _xcorr_max(W[best], W[i])
            uniq[best].aliases.append({"path": uniq[i].path, "reason": f"near-duplicate of {uniq[best].path} "
                                       f"(waveform correlation {c:.4f} over 50 ms, {uniq[i].rate} Hz vs {uniq[best].rate} Hz)"})
            uniq[best].aliases += uniq[i].aliases
            pairs.append((uniq[i].path, uniq[best].path, c))
    return sorted(keep), pairs


# ---- report ----------------------------------------------------------------------------------------------------------
def _report(ok, kept, rejected, not_audio, n_seen, n_exact, n_near, near_pairs, errors, reused, dirs, seconds) -> dict:
    def top(group):
        c = Counter(v for r in kept for v in r.tags[group])
        return [[k, n] for k, n in c.most_common(15)]
    pct = lambda g: round(100.0 * sum(1 for r in kept if r.tags[g]) / max(len(kept), 1), 1)
    n_rej = sum(len(v) for v in rejected.values())
    return {
        "dirs": dirs, "filesSeen": n_seen, "audioFiles": len(ok) + n_rej, "notAudio": dict(sorted(not_audio.items())),
        "accepted": len(ok), "exactDuplicates": n_exact, "nearDuplicates": n_near, "unique": len(kept),
        "rejected": {k: {"count": len(v), "examples": [x["path"] + (f"  ({x['detail']})" if x["detail"] else "") for x in v[:EXAMPLES]]}
                     for k, v in sorted(rejected.items())},
        "rejectedTotal": n_rej, "truncated": sum(1 for r in ok if r.truncated),
        "rates": {str(k): v for k, v in sorted(Counter(r.rate for r in ok).items())},
        "channels": {str(k): v for k, v in sorted(Counter(r.channels for r in ok).items())},
        "tagCoverage": {"cab": {"percent": pct("cab"), "top": top("cab")}, "mic": {"percent": pct("mic"), "top": top("mic")},
                        "position": {"percent": pct("position"), "top": top("position")}},
        "nearDuplicatePairs": [{"dropped": a, "kept": b, "correlation": round(c, 5)} for a, b, c in near_pairs[:EXAMPLES]],
        "indexReused": reused, "indexSeconds": round(seconds, 2), "errors": errors[:EXAMPLES],
    }


def format_report(r: dict) -> str:
    L = [f"IR scan of {', '.join(r['dirs'])}",
         f"  files seen {r['filesSeen']}, audio files {r['audioFiles']} (not audio: {r['notAudio'] or 'none'})",
         f"  accepted {r['accepted']}: exact duplicates {r['exactDuplicates']}, near duplicates {r['nearDuplicates']}, "
         f"unique {r['unique']}",
         f"  rejected {r['rejectedTotal']}, truncated to 2 s {r['truncated']}",
         f"  sample rates {r['rates']}, channels {r['channels']}"]
    for k, v in r["rejected"].items():
        L.append(f"  rejected {k}: {v['count']}")
        L += [f"      {p}" for p in v["examples"]]
    for g, v in r["tagCoverage"].items():
        L.append(f"  tag {g}: {v['percent']}% tagged; " + ", ".join(f"{k} {n}" for k, n in v["top"]))
    L.append(f"  index: {r['indexReused']} files reused from the cache, {r['indexSeconds']} s")
    for e in r["errors"]:
        L.append(f"  warning: {e}")
    return "\n".join(L)


def stderr_progress(done: int, total: int, eta: float) -> None:
    print(f"  indexing IRs: {done}/{total} files, ETA {int(eta)} s", file=sys.stderr, flush=True)


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m sawblade_match.matcher.irlib",
                                 description="Index IR directories and print what the matcher will see")
    ap.add_argument("--scan", action="append", required=True, metavar="DIR", help="directory to scan (repeatable)")
    ap.add_argument("--json", metavar="OUT", help="also write the report as JSON")
    ap.add_argument("--workers", type=int, default=4)
    a = ap.parse_args(argv)
    lib = scan(a.scan, workers=a.workers, progress=stderr_progress)
    print(format_report(lib.report))
    if a.json:
        Path(a.json).write_text(json.dumps(lib.report, indent=2) + "\n")
    return 0 if lib.records else 1


if __name__ == "__main__":
    sys.exit(main())

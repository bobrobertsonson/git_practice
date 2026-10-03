"""Candidate quality filter (spec section A2). Pure functions on Tone objects; no network.

Rules (all thresholds configurable and recorded in the pool manifest):
  * slot fit by gear: pedal -> pedal slot; amp -> amp slot (amp-only); cab (format ir) -> cab slot;
    amp-cab ("full rig") is never a slot candidate, only a reference.
  * architecture: A2 preferred; A1 only when the tone has no A2 models.
  * recency: published_at (fallback updated_at) within `max_age_months`.
  * popularity: favorites_count and downloads_count >= absolute floors (default 100 / 1000). An
    optional per-gear percentile over the fetched set (`popularity_percentile`, default off) raises
    the bar. Favorited tones below the bar are excluded unless `keep_favorites_below_floor`
    (then kept and flagged).
"""
from __future__ import annotations

from dataclasses import dataclass, field, asdict
from datetime import datetime, timedelta, timezone
from typing import Any, Iterable

import numpy as np

from .licenses import license_problem
from .types import Tone

DAYS_PER_MONTH = 30.4375


@dataclass
class FilterConfig:
    max_age_months: float = 18.0
    popularity_percentile: float | None = None   # opt-in
    min_favorites: int = 100         # absolute floors (lead-set; applied on top of the percentile)
    min_downloads: int = 1000
    allow_a1_fallback: bool = True
    favorites_bypass_recency: bool = False
    keep_favorites_below_floor: bool = False


@dataclass
class Decision:
    tone: Tone
    sources: list[str]
    slot: str | None = None           # "pedal" | "amp" | "cab" | "reference" | None
    status: str = "excluded"          # "included" | "excluded" | "reference"
    reasons: list[str] = field(default_factory=list)
    flags: list[str] = field(default_factory=list)
    architecture: str | None = None   # preferred architecture ("2"/"1") for NAM tones
    thresholds: dict[str, float] = field(default_factory=dict)
    models: list[dict[str, Any]] = field(default_factory=list)   # ALL candidate models of the tone
    downloads: list[dict[str, Any]] = field(default_factory=list)  # {model_id, path, sha256}

    @property
    def favorited(self) -> bool:
        return "favorited" in self.sources


def parse_ts(s: str | None) -> datetime | None:
    if not s:
        return None
    try:
        d = datetime.fromisoformat(s.replace("Z", "+00:00"))
    except ValueError:
        return None
    return d if d.tzinfo else d.replace(tzinfo=timezone.utc)


def slot_of(t: Tone) -> str | None:
    if t.gear == "amp-cab" or t.gear == "full-rig":
        return "reference"
    if t.gear == "pedal" and t.format == "nam":
        return "pedal"
    if t.gear == "amp" and t.format == "nam":
        return "amp"
    if t.gear == "cab" and t.format == "ir":
        return "cab"
    return None


def popularity_thresholds(tones: Iterable[Tone], cfg: FilterConfig) -> dict[str, dict[str, float]]:
    """Per-gear thresholds from the whole fetched (de-duplicated) set."""
    by_gear: dict[str, list[Tone]] = {}
    for t in tones:
        by_gear.setdefault(t.gear, []).append(t)
    out = {}
    for gear, ts in by_gear.items():
        fav, dl = float(cfg.min_favorites), float(cfg.min_downloads)
        out[gear] = {"favorites": fav, "downloads": dl, "n": len(ts)}
        if cfg.popularity_percentile is not None:
            pf = float(np.percentile([t.favorites_count for t in ts], cfg.popularity_percentile))
            pd = float(np.percentile([t.downloads_count for t in ts], cfg.popularity_percentile))
            out[gear].update(favorites=max(pf, fav), downloads=max(pd, dl),
                             favorites_percentile_value=pf, downloads_percentile_value=pd)
    return out


def evaluate(
    tones: dict[int, Tone],
    sources: dict[int, list[str]],
    cfg: FilterConfig,
    now: datetime,
    gears: Iterable[str] | None = None,
) -> tuple[list[Decision], dict[str, dict[str, float]]]:
    """Decide every tone. ``gears`` (slot names pedal/amp/cab) restricts slot candidates."""
    thresholds = popularity_thresholds(tones.values(), cfg)
    wanted = set(gears) if gears else None
    cutoff = now - timedelta(days=cfg.max_age_months * DAYS_PER_MONTH)
    decisions: list[Decision] = []
    for tid in sorted(tones):
        t = tones[tid]
        d = Decision(t, sorted(sources.get(tid, [])))
        decisions.append(d)

        # slot fit
        d.slot = slot_of(t)
        if d.slot == "reference":
            d.status = "reference"
            d.reasons.append("full_rig_reference_only")
            continue
        if d.slot is None:
            d.reasons.append(f"not_slot_eligible:gear={t.gear},format={t.format}")
            continue
        if wanted is not None and d.slot not in wanted:
            d.reasons.append(f"slot_not_requested:{d.slot}")
            continue

        # license (hard rule, no favorites bypass)
        lp = license_problem(t.license)
        if lp:
            d.reasons.append(lp)

        # architecture / models
        if t.format == "nam":
            if t.a2_models_count > 0:
                d.architecture = "2"
            elif t.a1_models_count > 0 and cfg.allow_a1_fallback:
                d.architecture = "1"
                d.flags.append("a1_only")
            else:
                d.reasons.append("no_a2_models" if t.a1_models_count == 0 else "a1_fallback_disabled")
        elif t.irs_count + t.models_count == 0:
            d.reasons.append("no_models")

        # recency
        date = parse_ts(t.published_at) or parse_ts(t.updated_at)
        if date is None:
            d.reasons.append("no_publish_date")
        elif date < cutoff and not (cfg.favorites_bypass_recency and d.favorited):
            d.reasons.append(f"too_old:{date.date().isoformat()}")

        # popularity
        th = thresholds[t.gear]
        d.thresholds = {"favorites": th["favorites"], "downloads": th["downloads"]}
        low = []
        if t.favorites_count < th["favorites"]:
            low.append(f"favorites {t.favorites_count} < {th['favorites']:g}")
        if t.downloads_count < th["downloads"]:
            low.append(f"downloads {t.downloads_count} < {th['downloads']:g}")
        if low:
            if d.favorited and cfg.keep_favorites_below_floor:
                d.flags.append("below_popularity_floor")
            else:
                d.reasons.append("below_popularity:" + "; ".join(low))

        if not d.reasons:
            d.status = "included"
    return decisions, thresholds


def config_dict(cfg: FilterConfig) -> dict[str, Any]:
    return asdict(cfg)

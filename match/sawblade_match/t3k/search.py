"""Read-only search view: tones/search results annotated with the pool quality filter verdict."""
from __future__ import annotations

from datetime import datetime
from typing import Any

from .filter import FilterConfig, evaluate
from .types import Tone


def assess(tones: list[Tone], cfg: FilterConfig, now: datetime) -> list[dict[str, Any]]:
    """One record per result (API order) with ``passes`` and the failing ``reasons``.

    Uses the same ``evaluate`` as ``pull`` (licence, slot fit, A2 preference, recency, popularity
    floors), so the verdict matches what ``pull --add-tone`` would do. Full rigs are ``reference``.
    """
    uniq = list(dict.fromkeys(t.id for t in tones))
    by_id = {t.id: t for t in tones}
    decisions, _ = evaluate({i: by_id[i] for i in uniq}, {i: ["search"] for i in uniq}, cfg, now)
    dec = {d.tone.id: d for d in decisions}
    out = []
    for i in uniq:
        t, d = by_id[i], dec[i]
        out.append({
            "tone_id": t.id, "title": t.title, "creator": t.user.creator, "gear": t.gear, "format": t.format,
            "license": t.license, "favorites_count": t.favorites_count, "downloads_count": t.downloads_count,
            "created_at": t.created_at or t.published_at, "models_count": t.models_count,
            "a2_models_count": t.a2_models_count, "a1_models_count": t.a1_models_count,
            "irs_count": t.irs_count, "sizes": list(t.sizes), "url": t.url,
            "passes": d.status == "included", "status": d.status, "reasons": d.reasons, "flags": d.flags,
        })
    return out


def _models_cell(r: dict[str, Any]) -> str:
    if r["format"] == "ir":
        return f'{r["irs_count"]} IR'
    return f'{r["models_count"]}m A2:{r["a2_models_count"]} A1:{r["a1_models_count"]}'


def table_rows(records: list[dict[str, Any]]) -> list[list[str]]:
    rows = []
    for r in records:
        verdict = "PASS" if r["passes"] else ("REF" if r["status"] == "reference" else "FAIL")
        why = "; ".join(r["reasons"] + r["flags"])
        rows.append([str(r["tone_id"]), r["title"][:40], r["creator"], r["license"] or "-", r["gear"],
                     f'{r["favorites_count"]}/{r["downloads_count"]}', (r["created_at"] or "-")[:10],
                     _models_cell(r), "/".join(r["sizes"]) or "-", verdict + (f" ({why})" if why else "")])
    return rows


TABLE_HEADER = ["tone", "title", "creator", "license", "gear", "fav/dl", "created", "models", "sizes", "quality"]

"""Typed views of the TONE3000 API v1 objects (see tone3000.com/api, "Types").

Only fields we use are modelled; the untouched JSON is kept in ``raw`` so the cache and the
pool manifest can store it verbatim.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


def _uid(v):
    """User ids are documented as int but the live API returns UUID strings; accept both."""
    if isinstance(v, int) or v is None:
        return v or 0
    return int(v) if str(v).isdigit() else str(v)


@dataclass(frozen=True)
class EmbeddedUser:
    id: int | str
    username: str
    display_name: str | None = None
    is_verified: bool = False
    url: str | None = None

    @property
    def creator(self) -> str:
        # display_name is only ever set for verified creators; fall back to username.
        return self.display_name or self.username

    @classmethod
    def from_json(cls, d: dict[str, Any] | None) -> "EmbeddedUser":
        d = d or {}
        return cls(
            id=_uid(d.get("id")),
            username=str(d.get("username") or ""),
            display_name=d.get("display_name"),
            is_verified=bool(d.get("is_verified", False)),
            url=d.get("url"),
        )


@dataclass(frozen=True)
class User:
    id: int | str
    username: str
    display_name: str | None
    raw: dict[str, Any] = field(repr=False, compare=False, default_factory=dict)

    @classmethod
    def from_json(cls, d: dict[str, Any]) -> "User":
        return cls(_uid(d["id"]), str(d.get("username") or ""), d.get("display_name"), d)


@dataclass(frozen=True)
class Tone:
    id: int
    title: str
    gear: str
    format: str
    license: str
    url: str
    user: EmbeddedUser
    created_at: str | None = None
    updated_at: str | None = None
    published_at: str | None = None
    description: str | None = None
    sizes: tuple[str, ...] = ()
    models_count: int = 0
    a1_models_count: int = 0
    a2_models_count: int = 0
    irs_count: int = 0
    custom_models_count: int = 0
    downloads_count: int = 0
    favorites_count: int = 0
    is_favorite: bool = False
    raw: dict[str, Any] = field(repr=False, compare=False, default_factory=dict)

    @classmethod
    def from_json(cls, d: dict[str, Any]) -> "Tone":
        def n(k: str) -> int:
            return int(d.get(k) or 0)

        return cls(
            id=int(d["id"]),
            title=str(d.get("title") or ""),
            gear=str(d.get("gear") or ""),
            format=str(d.get("format") or ""),
            license=str(d.get("license") or ""),
            url=str(d.get("url") or ""),
            user=EmbeddedUser.from_json(d.get("user")),
            created_at=d.get("created_at"),
            updated_at=d.get("updated_at"),
            published_at=d.get("published_at"),
            description=d.get("description"),
            sizes=tuple(d.get("sizes") or ()),
            models_count=n("models_count"),
            a1_models_count=n("a1_models_count"),
            a2_models_count=n("a2_models_count"),
            irs_count=n("irs_count"),
            custom_models_count=n("custom_models_count"),
            downloads_count=n("downloads_count"),
            favorites_count=n("favorites_count"),
            is_favorite=bool(d.get("is_favorite", False)),
            raw=d,
        )


@dataclass(frozen=True)
class Model:
    id: int
    tone_id: int
    name: str
    size: str
    model_url: str
    architecture_version: str | None
    raw: dict[str, Any] = field(repr=False, compare=False, default_factory=dict)

    @classmethod
    def from_json(cls, d: dict[str, Any]) -> "Model":
        av = d.get("architecture_version")
        return cls(
            id=int(d["id"]),
            tone_id=int(d.get("tone_id") or 0),
            name=str(d.get("name") or ""),
            size=str(d.get("size") or ""),
            model_url=str(d["model_url"]),
            architecture_version=None if av is None else str(av),
            raw=d,
        )

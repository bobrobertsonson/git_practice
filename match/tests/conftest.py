from __future__ import annotations

import json
import re
from datetime import datetime, timezone

import httpx
import pytest
import respx

from sawblade_match.t3k.auth import Session, TokenManager, TokenStore
from sawblade_match.t3k.client import T3KClient

BASE = "https://t3k.test"
NOW = datetime(2026, 10, 1, tzinfo=timezone.utc)
SECRET_ACCESS = "acc-SECRET-0001"
SECRET_REFRESH = "ref-SECRET-0001"


class FakeClock:
    def __init__(self):
        self.t = 1000.0
        self.sleeps: list[float] = []

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.sleeps.append(s)
        self.t += s


def tone_json(id, *, gear="amp", fmt="nam", a2=1, a1=0, fav=100, dl=1000, pub="2026-06-01T00:00:00Z",
              title=None, user="alice", license="cc-by", verified=False, **kw):
    d = {
        "id": id, "user_id": 1, "user": {"id": 7, "username": user, "display_name": None,
                                         "is_verified": verified, "avatar_url": None, "url": ""},
        "created_at": pub, "updated_at": pub, "published_at": pub, "title": title or f"Tone {id}",
        "description": "d", "gear": gear, "images": None, "is_public": True, "links": None,
        "format": fmt, "license": license, "sizes": ["standard"], "makes": [], "tags": [],
        "models_count": a2 + a1, "a1_models_count": a1, "a2_models_count": a2, "irs_count": 1 if fmt == "ir" else 0,
        "custom_models_count": 0, "downloads_count": dl, "favorites_count": fav, "is_favorite": False,
        "url": f"https://www.tone3000.com/tones/tone-{id}",
    }
    d.update(kw)
    return d


def model_json(id, tone_id, *, size="standard", arch="2", url=None):
    return {"id": id, "created_at": "x", "updated_at": "x", "user_id": 1,
            "model_url": url or f"{BASE}/api/v1/models/{id}/download", "name": f"m{id}", "size": size,
            "tone_id": tone_id, "architecture_version": arch}


class FakeApi:
    """Routes a minimal TONE3000 API on a respx mock. Records every API call."""

    def __init__(self, mock: respx.MockRouter):
        self.mock = mock
        self.calls: list[httpx.Request] = []
        self.favorited: list[dict] = []
        self.trending: dict[str, list[dict]] = {}
        self.latest: list[dict] = []
        self.search_results: list[dict] = []
        self.tones: dict[int, dict] = {}
        self.models: dict[int, list[dict]] = {}   # tone_id -> models (all architectures)
        self.files: dict[int, bytes] = {}
        mock.route(host="t3k.test", path__regex=r"^/api/v1/(?!oauth)").mock(side_effect=self._handle)

    def add_tone(self, t, models=()):
        self.tones[t["id"]] = t
        self.models[t["id"]] = list(models)
        for m in models:
            self.files[m["id"]] = f"FILE-{m['id']}".encode()

    def requests(self, pattern):
        return [r for r in self.calls if re.search(pattern, r.url.path)]

    def _page(self, items, req):
        q = req.url.params
        ps, pg = int(q.get("page_size", 10)), int(q.get("page", 1))
        total = len(items)
        return httpx.Response(200, json={"data": items[(pg - 1) * ps: pg * ps], "page": pg,
                                         "page_size": ps, "total": total,
                                         "total_pages": max(1, -(-total // ps))})

    def _handle(self, req: httpx.Request):
        self.calls.append(req)
        p, q = req.url.path, req.url.params
        if req.headers.get("authorization") != f"Bearer {SECRET_ACCESS}":
            return httpx.Response(401, json={"error": "unauthorized"})
        if p == "/api/v1/user":
            return httpx.Response(200, json={"id": 7, "username": "alice", "display_name": None})
        if p == "/api/v1/tones/favorited":
            return self._page(self.favorited, req)
        if p == "/api/v1/tones/trending":
            return httpx.Response(200, json={"data": self.trending.get(q.get("gear", ""), [])})
        if p == "/api/v1/tones/search":
            assert "architecture" in q
            return self._page(self.search_results, req)
        if p == "/api/v1/tones/latest":
            return httpx.Response(200, json={"data": self.latest})
        if m := re.fullmatch(r"/api/v1/tones/(\d+)", p):
            assert "architecture" in q
            if int(m[1]) not in self.tones:
                return httpx.Response(404, json={"error": "not found"})
            return httpx.Response(200, json=self.tones[int(m[1])])
        if p == "/api/v1/models":
            assert "architecture" in q, "architecture must always be passed"
            tid, arch = int(q["tone_id"]), q["architecture"]
            ms = [m for m in self.models[tid] if (m["architecture_version"] or "1") == arch
                  or self.tones[tid]["format"] == "ir"]
            return self._page(ms, req)
        if m := re.fullmatch(r"/api/v1/models/(\d+)", p):
            mid = int(m[1])
            for ms in self.models.values():
                for x in ms:
                    if x["id"] == mid:
                        return httpx.Response(200, json=x)
            return httpx.Response(404)
        if m := re.fullmatch(r"/api/v1/models/(\d+)/download", p):
            return httpx.Response(200, content=self.files[int(m[1])])
        return httpx.Response(404)


@pytest.fixture
def clock():
    return FakeClock()


@pytest.fixture
def store(tmp_path):
    return TokenStore(tmp_path / "cfg" / "t3k_tokens.json")


@pytest.fixture
def api(respx_mock):
    return FakeApi(respx_mock)


@pytest.fixture
def make_client(store, clock):
    def _make(access=SECRET_ACCESS, refresh=SECRET_REFRESH, expires_at=None, **kw):
        http = httpx.Client(base_url=BASE, timeout=5)
        tm = TokenManager("t3k_pub_test", http, store, now=clock)
        if access:
            tm.set_session(Session(access, refresh, clock() + 3600 if expires_at is None else expires_at))
        return T3KClient(tm, BASE, http=http, sleep=clock.sleep, clock=clock, **kw)
    return _make


@pytest.fixture(autouse=True)
def _isolated_pool_sources(tmp_path, monkeypatch):
    """Never read the developer's real ~/.config/sawblade/pool_sources.json."""
    monkeypatch.setenv("SAWBLADE_POOL_SOURCES", str(tmp_path / "no_such_pool_sources.json"))

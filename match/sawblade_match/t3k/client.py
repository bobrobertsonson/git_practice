"""TONE3000 API v1 client: rate limiting, retries, pagination, downloads.

Every endpoint needs an end-user Bearer token (see auth.py). Never logs tokens/headers.
"""
from __future__ import annotations

import hashlib
import logging
import os
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Callable, Iterator
from urllib.parse import urlparse

import httpx

from .auth import DEFAULT_BASE_URL, TokenManager
from .errors import ApiError, RateLimitedError, ReauthRequired
from .types import Model, Tone, User

log = logging.getLogger("sawblade.t3k")

ARCH_A1, ARCH_A2, ARCH_CUSTOM = "1", "2", "custom"
DEPRECATION_HEADER = "X-Tone3000-Deprecations"


class TokenBucket:
    """Classic token bucket; ``acquire`` blocks (via injected sleep) until a token is free."""

    def __init__(self, capacity: float, per_seconds: float,
                 clock: Callable[[], float] = time.monotonic,
                 sleep: Callable[[float], None] = time.sleep):
        self.capacity = float(capacity)
        self.rate = capacity / per_seconds
        self._clock, self._sleep = clock, sleep
        self._tokens = float(capacity)
        self._last = clock()
        self._lock = threading.Lock()

    def acquire(self) -> None:
        while True:
            with self._lock:
                t = self._clock()
                self._tokens = min(self.capacity, self._tokens + (t - self._last) * self.rate)
                self._last = t
                if self._tokens >= 1.0 - 1e-9:
                    self._tokens -= 1.0
                    return
                wait = (1.0 - self._tokens) / self.rate
            self._sleep(wait)


class T3KClient:
    def __init__(
        self,
        tokens: TokenManager,
        base_url: str = DEFAULT_BASE_URL,
        *,
        http: httpx.Client | None = None,
        rate_per_min: int = 100,
        search_rate_per_min: int = 10,
        max_retries: int = 5,
        backoff_base_s: float = 1.0,
        backoff_max_s: float = 60.0,
        sleep: Callable[[float], None] = time.sleep,
        clock: Callable[[], float] = time.monotonic,
    ):
        self.base_url = base_url.rstrip("/")
        self._host = urlparse(self.base_url).netloc
        self.tokens = tokens
        self._http = http or httpx.Client(base_url=self.base_url, timeout=30.0, follow_redirects=True)
        self._sleep = sleep
        self._bucket = TokenBucket(rate_per_min, 60.0, clock, sleep)
        self._search_bucket = TokenBucket(search_rate_per_min, 60.0, clock, sleep)
        self.max_retries, self.backoff_base_s, self.backoff_max_s = max_retries, backoff_base_s, backoff_max_s
        self._seen_deprecations: set[str] = set()

    # ---- low level -------------------------------------------------------------------
    def _url(self, path_or_url: str) -> str:
        return path_or_url if path_or_url.startswith("http") else f"{self.base_url}{path_or_url}"

    def _send(self, method: str, url: str, params: dict | None, *, stream: bool, search: bool,
              authed: bool = True) -> httpx.Response:
        refreshed = False
        attempt = 0
        while True:
            self._bucket.acquire()
            if search:
                self._search_bucket.acquire()
            headers: dict[str, str] = {}
            token = None
            if authed:
                token = self.tokens.get_access_token()
                headers["Authorization"] = f"Bearer {token}"
            req = self._http.build_request(method, url, params=params, headers=headers)
            try:
                resp = self._http.send(req, stream=stream)
            except (httpx.ConnectError, httpx.ReadError, httpx.RemoteProtocolError, httpx.TimeoutException) as e:
                if attempt >= self.max_retries:
                    raise
                delay = min(self.backoff_base_s * (2 ** attempt), self.backoff_max_s)
                attempt += 1
                log.warning("transport error (%s) from %s; retry %d/%d in %.1fs", type(e).__name__,
                            urlparse(url).path, attempt, self.max_retries, delay)
                self._sleep(delay)
                continue
            self._log_deprecations(resp)
            if resp.status_code == 401 and authed and not refreshed:
                resp.close()
                refreshed = True
                self.tokens.force_refresh(token)
                continue
            if resp.status_code == 429 or resp.status_code in (502, 503, 504):
                if attempt >= self.max_retries:
                    status, body = resp.status_code, "" if stream else resp.text[:200]
                    resp.close()
                    if status == 429:
                        raise RateLimitedError(status, "rate limit persisted after retries")
                    raise ApiError(status, body)
                delay = self._retry_delay(resp, attempt)
                resp.close()
                attempt += 1
                log.warning("HTTP %s from %s; retry %d/%d in %.1fs", resp.status_code,
                            urlparse(url).path, attempt, self.max_retries, delay)
                self._sleep(delay)
                continue
            if resp.status_code == 401:
                resp.close()
                raise ReauthRequired("API rejected the token after refresh; run `sawblade-t3k login`")
            if resp.status_code >= 400:
                body = "" if stream else resp.text[:200]
                resp.close()
                raise ApiError(resp.status_code, body)
            return resp

    def _retry_delay(self, resp: httpx.Response, attempt: int) -> float:
        ra = resp.headers.get("Retry-After")
        if ra:
            try:
                return min(float(ra), self.backoff_max_s)
            except ValueError:
                pass
        return min(self.backoff_base_s * (2 ** attempt), self.backoff_max_s)

    def _log_deprecations(self, resp: httpx.Response) -> None:
        v = resp.headers.get(DEPRECATION_HEADER)
        if v and v not in self._seen_deprecations:
            self._seen_deprecations.add(v)
            log.warning("TONE3000 deprecation notice (%s): %s", DEPRECATION_HEADER, v)

    def _get_json(self, path: str, params: dict | None = None, *, search: bool = False) -> Any:
        resp = self._send("GET", self._url(path), params, stream=False, search=search)
        return resp.json()

    def _paginate(self, path: str, params: dict | None = None, page_size: int = 100,
                  limit: int | None = None, search: bool = False) -> Iterator[dict]:
        page, n = 1, 0
        while True:
            q = dict(params or {}, page=page, page_size=page_size)
            body = self._get_json(path, q, search=search)
            data = body.get("data") or []
            for item in data:
                yield item
                n += 1
                if limit is not None and n >= limit:
                    return
            if not data or page >= int(body.get("total_pages") or 1):
                return
            page += 1

    # ---- endpoints -------------------------------------------------------------------
    def get_user(self) -> User:
        return User.from_json(self._get_json("/api/v1/user"))

    def _tones(self, path: str, gear: str | None, query: str | None, limit: int | None) -> list[Tone]:
        params = {k: v for k, v in (("gear", gear), ("query", query)) if v}
        return [Tone.from_json(d) for d in self._paginate(path, params, 100, limit)]

    def list_favorited(self, gear: str | None = None, query: str | None = None,
                       limit: int | None = None) -> list[Tone]:
        return self._tones("/api/v1/tones/favorited", gear, query, limit)

    def list_created(self, gear: str | None = None, query: str | None = None,
                     limit: int | None = None) -> list[Tone]:
        return self._tones("/api/v1/tones/created", gear, query, limit)

    def list_downloaded(self, gear: str | None = None, query: str | None = None,
                        limit: int | None = None) -> list[Tone]:
        return self._tones("/api/v1/tones/downloaded", gear, query, limit)

    def list_trending(self, gear: str | None = None) -> list[Tone]:
        """Top 10 trending tones (not paginated). Free-tier endpoint."""
        body = self._get_json("/api/v1/tones/trending", {"gear": gear} if gear else None)
        return [Tone.from_json(d) for d in body.get("data") or []]

    def list_latest(self) -> list[Tone]:
        """10 most recently published tones (not paginated). Free-tier endpoint."""
        body = self._get_json("/api/v1/tones/latest")
        return [Tone.from_json(d) for d in body.get("data") or []]

    def get_tone(self, tone_id: int | str, architecture: str = ARCH_A2) -> Tone:
        # architecture only affects models_count; per-arch counts are always returned.
        return Tone.from_json(self._get_json(f"/api/v1/tones/{tone_id}", {"architecture": architecture}))

    def get_model(self, model_id: int | str) -> Model:
        return Model.from_json(self._get_json(f"/api/v1/models/{model_id}"))

    def list_models(self, tone_id: int | str, architecture: str) -> list[Model]:
        """Models of a tone. ``architecture`` is mandatory: omitting it returns the legacy A1 set."""
        params = {"tone_id": tone_id, "architecture": architecture}
        return [Model.from_json(d) for d in self._paginate("/api/v1/models", params, page_size=300)]

    def search(self, query: str = "", *, gears: str | None = None, format: str | None = None,
               architecture: str = ARCH_A2, sort: str | None = None, calibrated: bool | None = None,
               verified: bool | None = None, limit: int = 25) -> list[Tone]:
        """Search tones (``/api/v1/tones/search``). OPT-IN ONLY.

        This endpoint is outside the free tier: Sawblade is a commercial product, so a signed
        commercial agreement with TONE3000 is REQUIRED before shipping anything that calls it
        (free tier = OAuth prompt flows + favorited/downloaded/created/trending/latest). It is also
        heavily rate limited, hence the separate, tighter client-side bucket.
        """
        params: dict[str, Any] = {"query": query, "architecture": architecture}
        if gears: params["gears"] = gears
        if format: params["format"] = format
        if sort: params["sort"] = sort
        if calibrated: params["calibrated"] = "true"
        if verified: params["verified"] = "true"
        return [Tone.from_json(d) for d in
                self._paginate("/api/v1/tones/search", params, 25, limit, search=True)]

    # ---- downloads -------------------------------------------------------------------
    def download_model(self, model_url: str, dest: Path) -> str:
        """Stream ``model_url`` to ``dest`` (temp file + rename). Returns the sha256 hex digest.

        The Bearer token is only attached for the TONE3000 host (httpx also drops it on
        cross-origin redirects), so a foreign model_url can never receive it.
        """
        dest = Path(dest)
        dest.parent.mkdir(parents=True, exist_ok=True)
        authed = urlparse(model_url).netloc in ("", self._host)
        if not authed:
            log.warning("model_url host differs from API host; fetching without credentials")
        resp = self._send("GET", self._url(model_url), None, stream=True, search=False, authed=authed)
        h = hashlib.sha256()
        fd, tmp = tempfile.mkstemp(dir=dest.parent, prefix=f".{dest.name}.")
        try:
            with os.fdopen(fd, "wb") as f:
                for chunk in resp.iter_bytes(65536):
                    h.update(chunk)
                    f.write(chunk)
            os.replace(tmp, dest)
        except BaseException:
            try:
                os.unlink(tmp)
            except FileNotFoundError:
                pass
            raise
        finally:
            resp.close()
        return h.hexdigest()

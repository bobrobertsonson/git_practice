"""OAuth 2.0 Device Authorization Grant (RFC 8628), token persistence and refresh.

Tokens are never logged or printed from here. The token file is created 0600 (0700 dir).
"""
from __future__ import annotations

import json
import logging
import os
import tempfile
import threading
import time
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Callable

import httpx

from .errors import AuthError, DeviceFlowError, ReauthRequired, T3KError

log = logging.getLogger("sawblade.t3k.auth")

DEFAULT_BASE_URL = "https://www.tone3000.com"
DEVICE_GRANT = "urn:ietf:params:oauth:grant-type:device_code"
REFRESH_MARGIN_S = 60.0


def default_token_path() -> Path:
    env = os.environ.get("SAWBLADE_T3K_TOKEN_FILE")
    if env:
        return Path(env)
    return Path.home() / ".config" / "sawblade" / "t3k_tokens.json"


SECRET_KEY_PREFIX = "t3k_cs_"


def publishable_client_id(value: object) -> str | None:
    """The id if it is a usable publishable client id; None for empty/non-string/secret-looking values."""
    if not isinstance(value, str):
        return None
    v = value.strip()
    if not v or v.startswith(SECRET_KEY_PREFIX):
        return None
    return v


@dataclass
class Session:
    access_token: str
    refresh_token: str
    expires_at: float  # wall-clock epoch seconds
    token_type: str = "bearer"
    scope: str | None = None
    client_id: str | None = None  # publishable id used at login; read by the C++ settings. Optional.

    def __repr__(self) -> str:  # never leak tokens through repr/tracebacks
        return f"Session(expires_at={self.expires_at}, scope={self.scope!r})"

    @classmethod
    def from_token_response(cls, body: dict, now: float, prev_refresh: str | None = None) -> "Session":
        refresh = body.get("refresh_token") or prev_refresh
        if not body.get("access_token") or not refresh:
            raise T3KError("token response missing access_token/refresh_token")
        return cls(
            access_token=body["access_token"],
            refresh_token=refresh,
            expires_at=now + float(body.get("expires_in") or 0),
            token_type=body.get("token_type") or "bearer",
            scope=body.get("scope"),
        )


class TokenStore:
    def __init__(self, path: Path | None = None):
        self.path = Path(path) if path else default_token_path()

    def load(self) -> Session | None:
        try:
            d = json.loads(self.path.read_text())
            # Our format has expires_at; the lead's login script writes obtained_at + expires_in.
            exp = float(d["expires_at"]) if "expires_at" in d else float(d["obtained_at"]) + float(d["expires_in"])
            return Session(d["access_token"], d["refresh_token"], exp,
                           d.get("token_type", "bearer"), d.get("scope"),
                           publishable_client_id(d.get("client_id")))
        except (FileNotFoundError, KeyError, ValueError):
            return None

    def save(self, s: Session) -> None:
        parent = self.path.parent
        created = not parent.exists()
        parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        if created or parent.name == "sawblade":   # never chmod an arbitrary pre-existing directory
            os.chmod(parent, 0o700)
        fd, tmp = tempfile.mkstemp(dir=self.path.parent, prefix=".t3k_tokens.")
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "w") as f:
                d = asdict(s)
                cid = publishable_client_id(d.pop("client_id", None))
                if cid:
                    d["client_id"] = cid
                json.dump(d, f)
            os.replace(tmp, self.path)
        except BaseException:
            try:
                os.unlink(tmp)
            except FileNotFoundError:
                pass
            raise

    def clear(self) -> None:
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass


@dataclass
class DeviceCode:
    device_code: str
    user_code: str
    verification_uri: str
    verification_uri_complete: str | None
    expires_in: float
    interval: float

    def __repr__(self) -> str:
        return f"DeviceCode(user_code={self.user_code!r})"


def _error_of(resp: httpx.Response) -> tuple[str, str]:
    try:
        b = resp.json()
        return str(b.get("error") or ""), str(b.get("error_description") or "")
    except ValueError:
        return "", ""


def request_device_code(http: httpx.Client, client_id: str, scope: str = "read") -> DeviceCode:
    # Device flow takes only client_id (+ scope): no redirect_uri / PKCE / prompt.
    r = http.post("/api/v1/oauth/device_authorization", data={"client_id": client_id, "scope": scope})
    if r.status_code != 200:
        err, desc = _error_of(r)
        raise DeviceFlowError(f"device authorization failed (HTTP {r.status_code}): {err} {desc}".strip())
    b = r.json()
    return DeviceCode(b["device_code"], b["user_code"], b["verification_uri"],
                      b.get("verification_uri_complete"), float(b.get("expires_in", 600)),
                      float(b.get("interval", 5)))


def poll_for_session(
    http: httpx.Client,
    client_id: str,
    dc: DeviceCode,
    *,
    sleep: Callable[[float], None] = time.sleep,
    now: Callable[[], float] = time.time,
    mono: Callable[[], float] = time.monotonic,
) -> Session:
    """Poll the token endpoint per RFC 8628 until approved, denied or expired."""
    interval = dc.interval
    deadline = mono() + dc.expires_in
    transient = 0
    while True:
        sleep(interval)
        if mono() > deadline:
            raise DeviceFlowError("device code expired; run login again")
        try:
            r = http.post("/api/v1/oauth/token", data={
                "grant_type": DEVICE_GRANT, "device_code": dc.device_code, "client_id": client_id})
        except httpx.TransportError:
            transient += 1
            if transient > 5:
                raise
            continue
        if r.status_code in (502, 503, 504):
            transient += 1
            if transient > 5:
                raise DeviceFlowError(f"token endpoint unavailable (HTTP {r.status_code})")
            continue
        transient = 0
        if r.status_code == 200:
            return Session.from_token_response(r.json(), now())
        err, desc = _error_of(r)
        if err == "authorization_pending":
            continue
        if err == "slow_down" or r.status_code == 429:
            interval += 5
            continue
        if err == "access_denied":
            raise DeviceFlowError("the user declined the request")
        if err == "expired_token":
            raise DeviceFlowError("device code expired or already redeemed; run login again")
        raise DeviceFlowError(f"device flow failed (HTTP {r.status_code}): {err} {desc}".strip())


class TokenManager:
    """Provides a valid access token; refreshes ~60 s early, one refresh at a time."""

    def __init__(
        self,
        client_id: str,
        http: httpx.Client,
        store: TokenStore | None = None,
        env_refresh_token: str | None = None,
        now: Callable[[], float] = time.time,
    ):
        self.client_id = client_id
        self._http = http
        self.store = store or TokenStore()
        self._seed = env_refresh_token
        self._now = now
        self._lock = threading.Lock()
        self._session: Session | None = None

    def set_session(self, s: Session) -> None:
        with self._lock:
            self._session = s
            self.store.save(s)

    def _current(self) -> Session | None:
        if self._session is None:
            self._session = self.store.load()  # stored tokens win over the env seed
        return self._session

    def get_access_token(self) -> str:
        with self._lock:
            s = self._current()
            if s is None:
                if not self._seed:
                    raise ReauthRequired("not logged in; run `sawblade-t3k login`")
                s = self._refresh(self._seed)
            elif s.expires_at - self._now() <= REFRESH_MARGIN_S:
                s = self._refresh(s.refresh_token)
            return s.access_token

    def force_refresh(self, stale_access_token: str | None = None) -> str:
        """Refresh after a 401. If another caller already rotated the token, reuse it."""
        with self._lock:
            s = self._current()
            if s is not None and stale_access_token and s.access_token != stale_access_token:
                return s.access_token
            rt = s.refresh_token if s else self._seed
            if not rt:
                raise ReauthRequired("not logged in; run `sawblade-t3k login`")
            return self._refresh(rt).access_token

    def _refresh(self, refresh_token: str) -> Session:
        r = self._http.post("/api/v1/oauth/token", data={
            "grant_type": "refresh_token", "refresh_token": refresh_token, "client_id": self.client_id})
        if r.status_code in (400, 401):
            self._session = None
            self.store.clear()
            raise ReauthRequired("session expired or revoked; run `sawblade-t3k login`")
        if r.status_code != 200:
            raise AuthError(f"token refresh failed: HTTP {r.status_code}")
        s = Session.from_token_response(r.json(), self._now(), prev_refresh=refresh_token)
        prev = self._session
        if prev is not None and prev.client_id:
            s.client_id = prev.client_id  # keep the login's client id across rotations
        self._session = s
        self.store.save(s)  # persist rotation immediately
        log.debug("access token refreshed (expires_at=%s)", s.expires_at)
        return s

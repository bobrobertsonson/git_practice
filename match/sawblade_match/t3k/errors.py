"""Exception types for the TONE3000 client."""
from __future__ import annotations


class T3KError(Exception):
    """Base class for all errors raised by this package."""


class ReauthRequired(T3KError):
    """No usable session: the user must run `sawblade-t3k login`."""


class DeviceFlowError(T3KError):
    """The device authorization flow ended without tokens."""


class ApiError(T3KError):
    def __init__(self, status: int, message: str):
        super().__init__(f"HTTP {status}: {message}")
        self.status = status
        self.message = message


class RateLimitedError(ApiError):
    """429 persisted after all retries."""

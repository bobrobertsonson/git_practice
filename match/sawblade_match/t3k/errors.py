"""Exception types for the TONE3000 client."""
from __future__ import annotations


class T3KError(Exception):
    """Base class for all errors raised by this package."""


class AuthError(T3KError):
    """Not logged in / token refresh failed / client id missing (CLI JSON code `auth`)."""


class LicenseRefused(T3KError):
    """The capture's licence is not allowed by licenses.py (CLI JSON code `license`)."""


class NotFoundError(T3KError):
    """Tone or model id unknown (CLI JSON code `not_found`)."""


class ReauthRequired(AuthError):
    """No usable session: the user must run `sawblade-t3k login`."""


class DeviceFlowError(AuthError):
    """The device authorization flow ended without tokens."""


class ApiError(T3KError):
    def __init__(self, status: int, message: str):
        super().__init__(f"HTTP {status}: {message}")
        self.status = status
        self.message = message


class RateLimitedError(ApiError):
    """429 persisted after all retries."""

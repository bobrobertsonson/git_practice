"""Strict id validation: ids end up in cache paths and URLs, so only digits are accepted."""
from __future__ import annotations

import re

from .errors import T3KError

_ID = re.compile(r"[0-9]+")


def require_id(value, what: str = "id") -> str:
    s = str(value)
    if isinstance(value, bool) or not _ID.fullmatch(s):
        raise T3KError(f"invalid {what} {s[:40]!r}: expected digits only")
    return s

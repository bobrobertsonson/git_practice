"""Capture license policy (CLAUDE.md "Capture licensing"). Sawblade is commercial: no cc-by-nc*.

Deliberately has no bypass and no CLI flag.
"""
from __future__ import annotations

from .errors import T3KError

ALLOWED_LICENSES = frozenset({"t3k", "cc-by", "cc-by-sa", "cc-by-nd", "cco"})


def license_problem(lic: str | None) -> str | None:
    """None if usable, else a short reason code."""
    lic = (lic or "").strip()
    if lic in ALLOWED_LICENSES:
        return None
    if lic.startswith("cc-by-nc"):
        return f"non_commercial_license:{lic}"
    return f"unknown_license:{lic}"


def check_license(lic: str | None, what: str = "capture") -> None:
    p = license_problem(lic)
    if p:
        raise T3KError(f"{what} refused: {p} (Sawblade is commercial; allowed: "
                       f"{', '.join(sorted(ALLOWED_LICENSES))})")

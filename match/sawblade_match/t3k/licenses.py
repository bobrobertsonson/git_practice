"""Capture license policy (CLAUDE.md "Capture licensing").

Sawblade is a personal, non-commercial project (user decision 2026-10-03), so `cc-by-nc*` captures are allowed.
Anything derived from one (preset, export) is marked non-commercial downstream: filter records carry the
`non_commercial` flag, the plugin tags the preset and export with NON-COMMERCIAL. Unknown/empty licenses are still refused.

Deliberately has no bypass and no CLI flag.
"""
from __future__ import annotations

from .errors import LicenseRefused

COMMERCIAL_OK_LICENSES = frozenset({"t3k", "cc-by", "cc-by-sa", "cc-by-nd", "cco"})
NON_COMMERCIAL_LICENSES = frozenset({"cc-by-nc", "cc-by-nc-sa", "cc-by-nc-nd"})
ALLOWED_LICENSES = COMMERCIAL_OK_LICENSES | NON_COMMERCIAL_LICENSES


def is_non_commercial(lic: str | None) -> bool:
    return (lic or "").strip().startswith("cc-by-nc")


def license_problem(lic: str | None) -> str | None:
    """None if usable, else a short reason code."""
    lic = (lic or "").strip()
    if lic in ALLOWED_LICENSES:
        return None
    return f"unknown_license:{lic}"


def check_license(lic: str | None, what: str = "capture") -> None:
    p = license_problem(lic)
    if p:
        raise LicenseRefused(f"{what} refused: {p} (allowed: {', '.join(sorted(ALLOWED_LICENSES))}; "
                             f"cc-by-nc* captures are usable but anything derived is non-commercial)")

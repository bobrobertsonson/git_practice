"""`sawblade-t3k` command line: login, whoami, pull, resolve."""
from __future__ import annotations

import argparse
import logging
import os
import sys
from pathlib import Path
from typing import Sequence

import httpx

from .auth import (DEFAULT_BASE_URL, TokenManager, TokenStore, poll_for_session,
                   request_device_code)
from .cache import Cache
from .client import T3KClient
from .errors import T3KError
from .filter import FilterConfig
from .pool import build_pool, write_manifest
from .resolve import default_output, resolve_file

GEAR_TO_SLOT = {"amp": "amp", "pedal": "pedal", "ir": "cab", "cab": "cab"}


def _env_client_id() -> str:
    cid = os.environ.get("TONE3000_CLIENT_ID", "").strip()
    if not cid:
        raise T3KError("TONE3000_CLIENT_ID is not set (your publishable key, t3k_pub_...). See match/README.md")
    if cid.startswith("t3k_cs_"):
        raise T3KError("TONE3000_CLIENT_ID looks like a SECRET key (t3k_cs_...); use the publishable t3k_pub_ key")
    return cid


def _base_url() -> str:
    return os.environ.get("TONE3000_BASE_URL", DEFAULT_BASE_URL)


def make_client(http: httpx.Client | None = None) -> T3KClient:
    base = _base_url()
    http = http or httpx.Client(base_url=base, timeout=30.0, follow_redirects=True)
    tm = TokenManager(_env_client_id(), http, TokenStore(), os.environ.get("TONE3000_REFRESH_TOKEN") or None)
    return T3KClient(tm, base, http=http)


def cmd_login(args: argparse.Namespace) -> int:
    base = _base_url()
    cid = _env_client_id()
    http = httpx.Client(base_url=base, timeout=30.0)
    dc = request_device_code(http, cid)
    print(f"\nOpen  {dc.verification_uri}  and enter the code:  {dc.user_code}")
    if dc.verification_uri_complete:
        print(f"(or open {dc.verification_uri_complete})")
    print(f"Waiting for approval (expires in {int(dc.expires_in)} s)...", flush=True)
    session = poll_for_session(http, cid, dc)
    tm = TokenManager(cid, http, TokenStore())
    tm.set_session(session)
    print(f"Logged in. Tokens saved to {tm.store.path} (mode 0600).")
    print("\nContainers are ephemeral. To skip this login next time, save this refresh token as the\n"
          "TONE3000_REFRESH_TOKEN secret (shown once; treat it like a password):\n")
    print(session.refresh_token)
    return 0


def cmd_whoami(args: argparse.Namespace) -> int:
    u = make_client().get_user()
    print(f"{u.display_name or u.username} (@{u.username}, id {u.id})")
    return 0


def _table(rows: list[list[str]], header: list[str]) -> None:
    rows = [header] + rows
    widths = [max(len(str(r[i])) for r in rows) for i in range(len(header))]
    for i, r in enumerate(rows):
        print("  ".join(str(c).ljust(w) for c, w in zip(r, widths)).rstrip())
        if i == 0:
            print("  ".join("-" * w for w in widths))


def cmd_pull(args: argparse.Namespace) -> int:
    cfg = FilterConfig(max_age_months=args.max_age_months, popularity_percentile=args.percentile,
                       min_favorites=args.min_favorites, min_downloads=args.min_downloads,
                       allow_a1_fallback=not args.no_a1_fallback, prefer_size=args.prefer_size,
                       favorites_bypass_recency=args.favorites_bypass_recency)
    slots = sorted({GEAR_TO_SLOT[g] for g in args.gear}) if args.gear else ["pedal", "amp", "cab"]
    cache = Cache(Path(args.cache_dir) if args.cache_dir else None)
    if args.search is not None:
        print("NOTE: --search uses tones/search, which requires a commercial agreement with TONE3000 "
              "before shipping.", file=sys.stderr)
    m = build_pool(make_client(), cache, cfg, slots=slots, trending=not args.no_trending,
                   latest=not args.no_latest, search_query=args.search, download=not args.no_download)
    out = Path(args.manifest) if args.manifest else cache.root / "pool_manifest.json"
    write_manifest(m, out)
    rows = [[str(t["tone_id"]), t["slot"], t["title"][:40], t["creator"], t["license"],
             f'{t["favorites_count"]}/{t["downloads_count"]}',
             str((t["chosen_model"] or {}).get("architecture_queried", "-")),
             ",".join(t["flags"])] for t in m["tones"]]
    _table(rows, ["tone", "slot", "title", "creator", "license", "fav/dl", "arch", "flags"])
    c = m["counts"]
    print(f"\n{c.get('included', 0)} included, {c.get('reference', 0)} reference, "
          f"{c.get('excluded', 0)} excluded. Manifest: {out}")
    return 0


def cmd_resolve(args: argparse.Namespace) -> int:
    done = resolve_file(make_client(), Cache(Path(args.cache_dir) if args.cache_dir else None),
                        Path(args.preset), Path(args.output) if args.output else None,
                        prefer_size=args.prefer_size)
    dest = args.output or default_output(Path(args.preset))
    print(f"Resolved {len(done)} capture(s) -> {dest}")
    for p in done:
        print(f"  {p}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-t3k", description="TONE3000 access for Sawblade")
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("login", help="device-flow login (needs TONE3000_CLIENT_ID)").set_defaults(fn=cmd_login)
    sub.add_parser("whoami", help="show the logged-in TONE3000 user").set_defaults(fn=cmd_whoami)

    q = sub.add_parser("pull", help="build the filtered candidate pool and download it to the cache")
    q.add_argument("--favorites", action="store_true", help="(default, always on) include favorited tones")
    q.add_argument("--gear", nargs="+", choices=sorted(GEAR_TO_SLOT), help="slots to keep: amp pedal ir")
    q.add_argument("--no-trending", action="store_true")
    q.add_argument("--no-latest", action="store_true")
    q.add_argument("--search", metavar="QUERY", default=None,
                   help="OPT-IN tones/search (commercial agreement required before shipping)")
    q.add_argument("--no-download", action="store_true", help="write the manifest only")
    q.add_argument("--manifest", help="manifest path (default: <cache>/pool_manifest.json)")
    q.add_argument("--cache-dir")
    q.add_argument("--max-age-months", type=float, default=FilterConfig.max_age_months)
    q.add_argument("--percentile", type=float, default=FilterConfig.popularity_percentile)
    q.add_argument("--min-favorites", type=int, default=FilterConfig.min_favorites)
    q.add_argument("--min-downloads", type=int, default=FilterConfig.min_downloads)
    q.add_argument("--no-a1-fallback", action="store_true")
    q.add_argument("--prefer-size", default=FilterConfig.prefer_size)
    q.add_argument("--favorites-bypass-recency", action="store_true")
    q.set_defaults(fn=cmd_pull)

    r = sub.add_parser("resolve", help="fill capture file/sha256/source in a preset")
    r.add_argument("preset")
    r.add_argument("-o", "--output", help="output path (default: <name>.resolved.json next to PRESET)")
    r.add_argument("--cache-dir")
    r.add_argument("--prefer-size", default="standard")
    r.set_defaults(fn=cmd_resolve)
    return p


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(level=logging.INFO if args.verbose else logging.WARNING,
                        format="%(levelname)s %(name)s: %(message)s", stream=sys.stderr)
    # httpx logs request lines at INFO; keep it quiet (it never logs headers, but be conservative).
    logging.getLogger("httpx").setLevel(logging.WARNING)
    try:
        return args.fn(args)
    except T3KError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except httpx.HTTPError as e:
        print(f"error: network failure: {type(e).__name__}: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

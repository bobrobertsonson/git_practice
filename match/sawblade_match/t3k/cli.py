"""`sawblade-t3k` command line: login, whoami, pull, resolve."""
from __future__ import annotations

import argparse
import logging
import os
import json
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Sequence

import httpx

from .auth import (DEFAULT_BASE_URL, TokenManager, TokenStore, poll_for_session,
                   request_device_code)
from .cache import Cache
from .client import T3KClient
from .errors import ApiError, AuthError, LicenseRefused, NotFoundError, T3KError
from .fetch import ensure_capture, list_candidates
from .licenses import check_license
from .filter import FilterConfig
from .ids import require_id
from .pool import build_pool, write_manifest
from .resolve import default_output, resolve_file
from .search import TABLE_HEADER, assess, pool_records, table_rows
from .sources import default_path, load_pool_sources, merge_unique

PERSONAL_USE_NOTE = ("tones/search is outside TONE3000's free tier. This is a personal, non-commercial "
                     "project: check the API terms before sharing anything that uses search.")


def _filter_cfg(args: argparse.Namespace) -> FilterConfig:
    return FilterConfig(max_age_months=args.max_age_months, popularity_percentile=args.percentile,
                        min_favorites=args.min_favorites, min_downloads=args.min_downloads,
                        allow_a1_fallback=not args.no_a1_fallback,
                        favorites_bypass_recency=args.favorites_bypass_recency,
                        keep_favorites_below_floor=args.keep_favorites_below_floor)


def _slots(gear) -> list[str]:
    return sorted({GEAR_TO_SLOT[g] for g in gear}) if gear else ["pedal", "amp", "cab"]


def _tone_ids(values) -> list[int]:
    return [int(require_id(v, "tone id")) for v in values or []]

GEAR_TO_SLOT = {"amp": "amp", "pedal": "pedal", "ir": "cab", "cab": "cab"}


def _env_client_id() -> str:
    cid = os.environ.get("TONE3000_CLIENT_ID", "").strip()
    if not cid:
        raise AuthError("TONE3000_CLIENT_ID is not set (your publishable key, t3k_pub_...). See match/README.md")
    if cid.startswith("t3k_cs_"):
        raise AuthError("TONE3000_CLIENT_ID looks like a SECRET key (t3k_cs_...); use the publishable t3k_pub_ key")
    return cid


def _base_url() -> str:
    return os.environ.get("TONE3000_BASE_URL", DEFAULT_BASE_URL)


def make_client(http: httpx.Client | None = None) -> T3KClient:
    base = _base_url()
    http = http or httpx.Client(base_url=base, timeout=30.0, follow_redirects=True)
    tm = TokenManager(_env_client_id(), http, TokenStore(), os.environ.get("TONE3000_REFRESH_TOKEN") or None)
    return T3KClient(tm, base, http=http)


def _emit(obj: dict) -> None:
    """One compact JSON document on stdout, flushed so a reader sees events as they happen."""
    print(json.dumps(obj), flush=True)


def cmd_login(args: argparse.Namespace) -> int:
    base = _base_url()
    cid = _env_client_id()
    http = httpx.Client(base_url=base, timeout=30.0)
    dc = request_device_code(http, cid)
    events = args.json_events
    if events:
        _emit({"event": "device_code", "verification_uri": dc.verification_uri,
               "verification_uri_complete": dc.verification_uri_complete, "user_code": dc.user_code,
               "expires_in": int(dc.expires_in)})
    else:
        print(f"\nOpen  {dc.verification_uri}  and enter the code:  {dc.user_code}")
        if dc.verification_uri_complete:
            print(f"(or open {dc.verification_uri_complete})")
        print(f"Waiting for approval (expires in {int(dc.expires_in)} s)...", flush=True)
    session = poll_for_session(http, cid, dc)
    tm = TokenManager(cid, http, TokenStore())
    tm.set_session(session)
    if events:
        _emit({"event": "logged_in"})   # the refresh token is saved by TokenStore and never printed here
        return 0
    print(f"Logged in. Tokens saved to {tm.store.path} (mode 0600).")
    print("\nContainers are ephemeral. To skip this login next time, save this refresh token as the\n"
          "TONE3000_REFRESH_TOKEN secret (shown once; treat it like a password):\n")
    print(session.refresh_token)
    return 0


def cmd_whoami(args: argparse.Namespace) -> int:
    u = make_client().get_user()
    if args.json:
        _emit({"id": u.id, "username": u.username, "display_name": u.display_name})
        return 0
    print(f"{u.display_name or u.username} (@{u.username}, id {u.id})")
    return 0


def _table(rows: list[list[str]], header: list[str]) -> None:
    rows = [header] + rows
    widths = [max(len(str(r[i])) for r in rows) for i in range(len(header))]
    for i, r in enumerate(rows):
        print("  ".join(str(c).ljust(w) for c, w in zip(r, widths)).rstrip())
        if i == 0:
            print("  ".join("-" * w for w in widths))


def cmd_search(args: argparse.Namespace) -> int:
    """Read-only: list tones/search results with the quality-filter verdict. No manifest, no download."""
    slots = _slots(args.gear)
    gears = "_".join(slots) if args.gear else None
    tones = make_client().search(args.query, gears=gears, limit=args.limit)
    records = assess(tones, _filter_cfg(args), datetime.now(timezone.utc))
    if args.json:
        json.dump(records, sys.stdout, indent=2)
        print()
        return 0
    print(PERSONAL_USE_NOTE, file=sys.stderr)
    _table(table_rows(records), TABLE_HEADER)
    npass = sum(r["passes"] for r in records)
    print(f"\n{len(records)} result(s), {npass} pass the quality filter.")
    return 0


def _gear_names(gear) -> set[str] | None:
    """`--gear` values as TONE3000 `gear` names (cab == ir)."""
    return {"ir" if g == "cab" else g for g in gear} if gear else None


def cmd_models(args: argparse.Namespace) -> int:
    """Read-only: the candidate models of a tone (A2 then A1; IRs as `pull` does). No download."""
    client = make_client()
    tone = client.get_tone(require_id(args.tone_id, "tone id"))
    found = list_candidates(client, tone)
    arch, models = found if found else ("", [])
    _emit({"tone_id": tone.id, "architecture": arch,
           "models": [{"model_id": m.id, "name": m.name, "size": m.size or None} for m in models]})
    return 0


def cmd_fetch(args: argparse.Namespace) -> int:
    """Licence-check, pick a model, download into the cache (a hit downloads nothing), print the result."""
    cache = Cache(Path(args.cache_dir) if args.cache_dir else None)
    client = make_client()
    tone = client.get_tone(require_id(args.tone_id, "tone id"))
    check_license(tone.license, f"tone {tone.id}")            # before any model lookup or download
    found = list_candidates(client, tone)
    models = found[1] if found else []
    if args.model is not None:
        want = int(require_id(args.model, "model id"))
        model = next((m for m in models if m.id == want), None)
        if model is None:
            raise NotFoundError(f"model {want} is not one of tone {tone.id}'s candidate models "
                                f"({', '.join(str(m.id) for m in models) or 'none'})")
    elif models:
        model = models[0]
    else:
        raise NotFoundError(f"tone {tone.id} has no usable models")
    entry = ensure_capture(client, cache, tone, model)
    _emit({"tone_id": tone.id, "model_id": model.id, "path": str(Path(entry.path).resolve()),
           "sha256": entry.sha256, "kind": tone.format, "gear": tone.gear,
           "source": {"provider": "tone3000", "id": str(tone.id), "modelId": str(model.id), "url": tone.url,
                      "title": tone.title, "creator": tone.user.creator, "license": tone.license}})
    return 0


def cmd_list(args: argparse.Namespace) -> int:
    """Read-only listing of favorites (network) or the pool manifest (no network), `search` record shape."""
    gears = _gear_names(args.gear)
    if args.source == "favorites":
        tones = make_client().list_favorited(query=args.query or None)
        if gears:
            tones = [t for t in tones if t.gear in gears]
        records = assess(tones, _filter_cfg(args), datetime.now(timezone.utc))
    else:
        cache = Cache(Path(args.cache_dir) if args.cache_dir else None)
        try:
            manifest = json.loads((cache.root / "pool_manifest.json").read_text())
        except (FileNotFoundError, ValueError):
            manifest = None
        records = pool_records(manifest if isinstance(manifest, dict) else None)
        if gears:
            records = [r for r in records if r["gear"] in gears]
        if args.query:
            q = args.query.lower()
            records = [r for r in records if q in (r["title"] or "").lower() or q in (r["creator"] or "").lower()]
    records = records[:args.limit]
    if args.json:
        json.dump(records, sys.stdout, indent=2)
        print()
        return 0
    _table([[str(r["tone_id"]), str(r["title"])[:40], str(r["creator"]), str(r["license"] or "-"),
             str(r["gear"]), "PASS" if r["passes"] else "FAIL"] for r in records],
           ["tone", "title", "creator", "license", "gear", "quality"])
    return 0


def cmd_pull(args: argparse.Namespace) -> int:
    cfg = _filter_cfg(args)
    slots = _slots(args.gear)
    cache = Cache(Path(args.cache_dir) if args.cache_dir else None)
    src_path = default_path()
    extra = load_pool_sources(src_path)
    searches = merge_unique(args.search or [], extra.searches)
    add_tones = merge_unique(_tone_ids(args.add_tone), extra.tones)
    force_tones = _tone_ids(args.force_tone)
    print(f"Extra pool sources file: {src_path} ({len(extra.searches)} search(es), {len(extra.tones)} tone(s))")
    if searches:
        print(f"NOTE: {PERSONAL_USE_NOTE}", file=sys.stderr)
    m = build_pool(make_client(), cache, cfg, slots=slots, trending=not args.no_trending,
                   latest=not args.no_latest, searches=searches, add_tones=add_tones,
                   force_tones=force_tones, download=not args.no_download,
                   max_models_per_tone=args.max_models_per_tone)
    out = Path(args.manifest) if args.manifest else cache.root / "pool_manifest.json"
    write_manifest(m, out)
    rows = [[str(t["tone_id"]), t["slot"], t["title"][:40], t["creator"], t["license"],
             f'{t["favorites_count"]}/{t["downloads_count"]}',
             f'{len(t["models"])}m/{len(t["downloads"])}dl',
             ",".join(t["flags"])] for t in m["tones"]]
    _table(rows, ["tone", "slot", "title", "creator", "license", "fav/dl", "models", "flags"])
    c = m["counts"]
    print(f"\n{c.get('included', 0)} included, {c.get('reference', 0)} reference, "
          f"{c.get('excluded', 0)} excluded. Manifest: {out}")
    return 0


def cmd_resolve(args: argparse.Namespace) -> int:
    done = resolve_file(make_client(), Cache(Path(args.cache_dir) if args.cache_dir else None),
                        Path(args.preset), Path(args.output) if args.output else None,
                        first_model=args.first_model)
    dest = args.output or default_output(Path(args.preset))
    print(f"Resolved {len(done)} capture(s) -> {dest}")
    for p in done:
        print(f"  {p}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sawblade-t3k", description="TONE3000 access for Sawblade")
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)
    lg = sub.add_parser("login", help="device-flow login (needs TONE3000_CLIENT_ID)")
    lg.add_argument("--json-events", action="store_true",
                    help="print JSON event lines (device_code, logged_in) and never the refresh token")
    lg.set_defaults(fn=cmd_login, json=False)
    wh = sub.add_parser("whoami", help="show the logged-in TONE3000 user")
    wh.add_argument("--json", action="store_true", help="machine-readable output and errors")
    wh.set_defaults(fn=cmd_whoami)

    md = sub.add_parser("models", help="read-only: candidate models of a tone (A2 then A1), no download")
    md.add_argument("tone_id")
    md.add_argument("--json", action="store_true", help="machine-readable output and errors")
    md.add_argument("--cache-dir")
    md.set_defaults(fn=cmd_models)

    fe = sub.add_parser("fetch", help="download one capture into the cache and print where it is")
    fe.add_argument("tone_id")
    fe.add_argument("--model", metavar="MODEL_ID", help="one of `models`' ids (default: the first)")
    fe.add_argument("--json", action="store_true", help="machine-readable output and errors")
    fe.add_argument("--cache-dir")
    fe.set_defaults(fn=cmd_fetch)

    ls = sub.add_parser("list", help="read-only: list favorites or the cached pool, search-record shaped")
    ls.add_argument("--source", choices=["favorites", "pool"], required=True)
    ls.add_argument("--query", help="favorites: passed to the API; pool: title/creator substring")
    ls.add_argument("--gear", nargs="+", choices=sorted(GEAR_TO_SLOT), help="gear to keep: amp pedal ir")
    ls.add_argument("--limit", type=int, default=100)
    ls.add_argument("--json", action="store_true", help="machine-readable output and errors")
    ls.add_argument("--cache-dir")
    ls.add_argument("--max-age-months", type=float, default=FilterConfig.max_age_months)
    ls.add_argument("--popularity-percentile", "--percentile", dest="percentile", type=float, default=None,
                    metavar="P")
    ls.add_argument("--min-favorites", type=int, default=FilterConfig.min_favorites)
    ls.add_argument("--min-downloads", type=int, default=FilterConfig.min_downloads)
    ls.add_argument("--no-a1-fallback", action="store_true")
    ls.add_argument("--keep-favorites-below-floor", action="store_true")
    ls.add_argument("--favorites-bypass-recency", action="store_true")
    ls.set_defaults(fn=cmd_list)

    q = sub.add_parser("pull", help="build the filtered candidate pool and download it to the cache")
    q.add_argument("--favorites", action="store_true", help="(default, always on) include favorited tones")
    q.add_argument("--gear", nargs="+", choices=sorted(GEAR_TO_SLOT), help="slots to keep: amp pedal ir")
    q.add_argument("--no-trending", action="store_true")
    q.add_argument("--no-latest", action="store_true")
    q.add_argument("--search", metavar="QUERY", action="append", default=None,
                   help="OPT-IN tones/search query; repeatable (personal use: check the API terms before "
                        "sharing anything that uses search)")
    q.add_argument("--add-tone", metavar="ID", action="append", default=None,
                   help="add a specific tone id regardless of source (source 'lead-pick'); repeatable; "
                        "still subject to the licence and quality filter")
    q.add_argument("--force-tone", metavar="ID", action="append", default=None,
                   help="like --add-tone but skips the quality filter (recency/popularity); the licence "
                        "policy still applies. repeatable")
    q.add_argument("--no-download", action="store_true", help="write the manifest only")
    q.add_argument("--manifest", help="manifest path (default: <cache>/pool_manifest.json)")
    q.add_argument("--cache-dir")
    q.add_argument("--max-age-months", type=float, default=FilterConfig.max_age_months)
    q.add_argument("--popularity-percentile", "--percentile", dest="percentile", type=float, default=None,
                   metavar="P", help="opt-in: also require per-gear percentile P (default: floors only)")
    q.add_argument("--min-favorites", type=int, default=FilterConfig.min_favorites)
    q.add_argument("--min-downloads", type=int, default=FilterConfig.min_downloads)
    q.add_argument("--no-a1-fallback", action="store_true")
    q.add_argument("--keep-favorites-below-floor", action="store_true",
                   help="keep (and flag) favorited tones below the popularity floors")
    q.add_argument("--max-models-per-tone", type=int, default=3,
                   help="when downloading, fetch at most N models per tone (default 3; the manifest "
                        "always lists all models)")
    q.add_argument("--favorites-bypass-recency", action="store_true")
    q.set_defaults(fn=cmd_pull)

    sr = sub.add_parser("search", help="read-only tones/search listing with the quality-filter verdict")
    sr.add_argument("query")
    sr.add_argument("--gear", nargs="+", choices=sorted(GEAR_TO_SLOT), help="gear to search: amp pedal ir")
    sr.add_argument("--limit", type=int, default=25)
    sr.add_argument("--json", action="store_true", help="machine-readable output")
    sr.add_argument("--max-age-months", type=float, default=FilterConfig.max_age_months)
    sr.add_argument("--popularity-percentile", "--percentile", dest="percentile", type=float, default=None,
                    metavar="P")
    sr.add_argument("--min-favorites", type=int, default=FilterConfig.min_favorites)
    sr.add_argument("--min-downloads", type=int, default=FilterConfig.min_downloads)
    sr.add_argument("--no-a1-fallback", action="store_true")
    sr.add_argument("--keep-favorites-below-floor", action="store_true")
    sr.add_argument("--favorites-bypass-recency", action="store_true")
    sr.set_defaults(fn=cmd_search)

    r = sub.add_parser("resolve", help="fill capture file/sha256/source in a preset")
    r.add_argument("preset")
    r.add_argument("-o", "--output", help="output path (default: <name>.resolved.json next to PRESET)")
    r.add_argument("--cache-dir")
    r.add_argument("--first-model", action="store_true",
                   help="if a tone has several models and no source.modelId, use the first instead of failing")
    r.set_defaults(fn=cmd_resolve)
    return p


def error_code(e: Exception) -> str:
    """The `--json` error `code` for an exception: license|auth|not_found|network|error."""
    if isinstance(e, httpx.HTTPError):
        return "network"
    if isinstance(e, LicenseRefused):
        return "license"
    if isinstance(e, AuthError):
        return "auth"
    if isinstance(e, NotFoundError) or (isinstance(e, ApiError) and e.status in (404, 410)):
        return "not_found"
    return "error"


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(level=logging.INFO if args.verbose else logging.WARNING,
                        format="%(levelname)s %(name)s: %(message)s", stream=sys.stderr)
    # httpx logs request lines at INFO; keep it quiet (it never logs headers, but be conservative).
    logging.getLogger("httpx").setLevel(logging.WARNING)
    as_json = bool(getattr(args, "json", False) or getattr(args, "json_events", False))
    try:
        return args.fn(args)
    except (T3KError, httpx.HTTPError) as e:
        code, msg = error_code(e), str(e)
        if isinstance(e, httpx.HTTPError):
            msg = f"network failure: {type(e).__name__}: {e}"
        if as_json:
            _emit({"error": msg, "code": code})
        else:
            print(f"error: {msg}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

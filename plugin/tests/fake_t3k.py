#!/usr/bin/env python3
"""Stand-in for `sawblade-t3k` in the capture-browser tests (docs/specs/phase8_capture_browser.md).

Answers whoami / search / list / models / ladder / fetch / login from canned JSON by argv. Behaviour is steered by
environment variables:
  FAKE_T3K_MODE   ok (default) | error | sleep | garbage | exit1 | noisy
                  error: every command prints {"error", "code"} (FAKE_T3K_CODE, default "error") and exits 1
                  sleep: sleeps 60 s before answering (timeout tests)
                  garbage: prints non-JSON and exits 0;  exit1: prints a note on stderr and exits 1, no stdout
                  noisy: writes a stderr note before each answer (merged streams)
  FAKE_T3K_STATE  directory: if set, whoami fails with code "auth" until `login` has created <dir>/logged_in
  FAKE_T3K_FETCH  file returned as the fetched capture (default tests/fixtures/nam/linear_identity.nam)
  FAKE_T3K_KIND   kind reported by fetch (default nam)
  FAKE_T3K_MODELS_N  number: `models` answers that many models named "Gain 1"...
  FAKE_T3K_LOG    file: argv of every call is appended here
"""
import hashlib
import json
import os
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
args = sys.argv[1:]
mode = os.environ.get("FAKE_T3K_MODE", "ok")
state = os.environ.get("FAKE_T3K_STATE")
if os.environ.get("FAKE_T3K_LOG"):
    with open(os.environ["FAKE_T3K_LOG"], "a") as f:
        f.write(json.dumps(args) + "\n")


def out(obj):
    print(json.dumps(obj, indent=1))
    sys.exit(0)


def err(message, code):
    print(json.dumps({"error": message, "code": code}))
    sys.exit(1)


def rec(i, title, creator, lic, gear="pedal", passes=True, **kw):
    r = {"tone_id": i, "title": title, "creator": creator, "gear": gear, "format": "ir" if gear == "ir" else "nam",
         "license": lic, "favorites_count": 10 * i, "downloads_count": 100 * i, "created_at": "2026-07-26T10:00:00Z",
         "models_count": 2, "a2_models_count": 2, "a1_models_count": 0, "irs_count": 0, "sizes": ["standard"],
         "url": f"https://www.tone3000.com/tones/{i}", "passes": passes,
         "status": "included" if passes else "excluded", "reasons": [] if passes else ["below_popularity_floor"],
         "flags": ["non_commercial"] if lic.startswith("cc-by-nc") else []}
    r.update(kw)
    return r


RECORDS = [
    rec(101, "Boss HM-2w CHAINSAW", "ebheron", "t3k"),
    rec(102, "Swedish Chainsaw åäö", "sven", "cc-by"),
    rec(103, "Fuzz Drive NC", "nina", "cc-by-nc", passes=False),  # licence is fine (NC is allowed); fails the quality floor
    rec(104, "Tight Boost", "tom", "cco"),
    rec(105, "Dist Pedal SA", "sam", "cc-by-sa"),
    rec(106, "Brown Machine", "bo", "")  # empty licence: the UI shows a fallback,
]
IRS = [rec(201, "4x12 V30 SM57", "cabguy", "cc-by", gear="ir", models_count=0, a2_models_count=0, irs_count=6)]

if not args:
    err("no command", "error")
cmd = args[0]
if mode == "sleep":
    time.sleep(60)
if mode == "garbage":
    print("this is not json {")
    sys.exit(0)
if mode == "exit1":
    print("Traceback: something broke", file=sys.stderr)
    sys.exit(1)
if mode == "error" and cmd != "login":
    err("fake failure", os.environ.get("FAKE_T3K_CODE", "error"))
if mode == "noisy":
    print("note: talking to the API", file=sys.stderr, flush=True)

logged_in = state is None or os.path.exists(os.path.join(state, "logged_in"))
if cmd == "whoami":
    if not logged_in:
        err("not logged in", "auth")
    out({"id": 42, "username": "tester", "display_name": "Test User"})
elif cmd == "login":
    print(json.dumps({"event": "device_code", "verification_uri": "https://www.tone3000.com/device",
                      "verification_uri_complete": "https://www.tone3000.com/device?code=ABCD-1234", "user_code": "ABCD-1234",
                      "expires_in": 600}), flush=True)
    print("refresh_token=SECRETTOKEN123456", flush=True)
    time.sleep(float(os.environ.get("FAKE_T3K_LOGIN_DELAY", "0.3")))
    if state:
        open(os.path.join(state, "logged_in"), "w").write("1")
    print(json.dumps({"event": "logged_in"}), flush=True)
    sys.exit(0)
elif cmd in ("search", "list"):
    if not logged_in:
        err("not logged in", "auth")
    if cmd == "search" and ("--" not in args or args.index("--") != len(args) - 2):
        err("search: the query must follow '--'", "error")
    gear = args[args.index("--gear") + 1] if "--gear" in args else None
    recs = IRS if gear == "ir" else RECORDS
    if cmd == "list" and "--source" in args and args[args.index("--source") + 1] == "pool":
        recs = recs[:2]
    out(recs)
elif cmd == "models":
    tid = int(args[1])
    if tid == 999:
        err("unknown tone", "not_found")
    n = int(os.environ.get("FAKE_T3K_MODELS_N", "0"))
    if n:  # the pedalboard selector tests: n models "Gain 1".."Gain n"
        out({"tone_id": tid, "architecture": "A2", "models": [
            {"model_id": tid * 10 + i, "name": f"Gain {i}", "size": "standard"} for i in range(1, n + 1)]})
    out({"tone_id": tid, "architecture": "A2", "models": [
        {"model_id": tid * 10 + 1, "name": "Standard", "size": "standard"},
        {"model_id": tid * 10 + 2, "name": "Lite", "size": None}]})
elif cmd == "ladder":
    # v0.3 Task E: tone 101 has a 5-step ladder (own model 1011 is one of the rungs), every other tone none (rungs null).
    tid = args[1]
    if os.environ.get("FAKE_T3K_LADDER_FAIL"):
        err("not logged in", "auth")
    time.sleep(float(os.environ.get("FAKE_T3K_LADDER_SLEEP", "0")))
    gate = os.environ.get("FAKE_T3K_LADDER_GATE")
    if gate:  # blocks until the file exists (tests that need a ladder run to stay in flight)
        t0 = time.time()
        while not os.path.exists(gate) and time.time() - t0 < 60:
            time.sleep(0.02)
    rungs = None
    if tid == "101":
        rungs = [{"model_id": str(1010 + i), "gain": float(2 * i), "name": f"Gain {2 * i}"} for i in range(1, 6)]
    out({"tone_id": tid, "size": "standard", "rungs": rungs})
elif cmd == "fetch":
    tid = int(args[1])
    lic = {103: "cc-by-nc", 107: "unknown"}.get(tid, "cc-by")
    # Like the real CLI (match/sawblade_match/t3k/licenses.py): cc-by-nc* is usable (personal project, CLAUDE.md); only an
    # unknown licence is refused with code "license".
    if lic == "unknown":
        err(f"tone {tid} refused: unknown_license:unknown", "license")
    mid = int(args[args.index("--model") + 1]) if "--model" in args else tid * 10 + 1
    kind = os.environ.get("FAKE_T3K_KIND", "nam")
    default = os.path.join(ROOT, "tests", "fixtures", "nam", "linear_identity.nam")
    path = os.environ.get("FAKE_T3K_FETCH", default)
    out({"tone_id": tid, "model_id": mid, "path": path, "sha256": hashlib.sha256(open(path, "rb").read()).hexdigest(), "kind": kind, "gear": "pedal",
         "source": {"provider": "tone3000", "id": str(tid), "modelId": str(mid), "url": f"https://www.tone3000.com/tones/{tid}",
                    "title": "Fake " + str(tid), "creator": "fakecreator", "license": lic}})
else:
    err("unknown command " + cmd, "error")

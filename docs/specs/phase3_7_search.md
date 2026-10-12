# Phase 3.7: lead-driven TONE3000 search + pool expansion

The lead can now search TONE3000 directly (logged in as the user; personal, non-commercial
project, so the "commercial agreement before shipping" note becomes "check the API terms
before sharing anything that uses search").

## Changes (match-engineer, t3k/ only + README)
1. `sawblade-t3k search QUERY [--gear ...] [--json]` lists results with:
   - tone id, title, creator, licence, favorites/downloads, created date, model count, A2/size info;
   - whether the result passes the quality filter (≤18 months or favorites-bypass, ≥100 fav,
     ≥1000 dl, A2 preferred), with the failing reasons.

   It is read-only: no manifest change, no download.
2. `pull` accepts `--search` multiple times. It also accepts `--add-tone ID` (repeatable)
   to add specific tone ids regardless of source. Tones added this way still go through the
   licence and quality filter unless `--force-tone ID` is given, and the manifest records the
   source as `lead-pick`.
3. **Persistent extra sources:**
   - `~/.config/sawblade/pool_sources.json` (`{"searches": [...], "tones": [...]}`) is merged
     into every `pull`, so the pool is reproducible without repeating flags.
   - The file path is printed by `pull`.
   - The file is not committed: it is user state.
4. **Wording:** replace "commercial agreement required before shipping" with the personal-use
   wording above in the CLI help, the NOTE line and the README.
5. **Tests:** mocked HTTP only, no network. They cover:
   - multi-search merge with de-duplication;
   - `--add-tone`;
   - `pool_sources.json` merge;
   - `search` output formatting;
   - licence and quality filtering is still applied.

Out of scope: the licence policy (still blocks cc-by-nc* pending a user-run session).

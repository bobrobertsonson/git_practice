#!/usr/bin/env bash
# Fetch reference audio (song originals + Omega Station cover mixes) for matcher validation.
# Personal / educational evaluation only: the audio lands in testdata/ (git-ignored) and is
# never committed or redistributed (CLAUDE.md, docs/TEST_SOURCES.md).
#
#   scripts/fetch_references.sh            # dry run: show which video each entry resolves to
#   scripts/fetch_references.sh --go       # download everything not already on disk
#   scripts/fetch_references.sh --go nails__no_surrender   # only these slugs
#
# Needs yt-dlp and ffmpeg (Mac: brew install yt-dlp ffmpeg).
# Entries come from scripts/reference_sources.txt: "kind|slug|query-or-URL". A search query
# picks the top YouTube result, so run the dry run first and replace any wrong pick with
# the exact URL. Override the list with REFERENCE_SOURCES=<file>.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
list="${REFERENCE_SOURCES:-$repo/scripts/reference_sources.txt}"
ytdlp="${YTDLP:-yt-dlp}"

go=0
only=()
for a in "$@"; do
  case "$a" in
    --go) go=1 ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) only+=("$a") ;;
  esac
done

for tool in "$ytdlp" ffmpeg; do
  command -v "$tool" >/dev/null 2>&1 || { echo "error: $tool not found (brew install yt-dlp ffmpeg)" >&2; exit 1; }
done
[[ -f "$list" ]] || { echo "error: source list not found: $list" >&2; exit 1; }

wanted() {
  [[ -z "${only[*]-}" ]] && return 0
  local s
  for s in ${only[@]+"${only[@]}"}; do [[ "$s" == "$1" ]] && return 0; done
  return 1
}

# original -> testdata/originals/<slug>.wav ; cover -> testdata/multis/<slug>/cover_mix.wav
dest_for() {
  case "$1" in
    original) echo "$repo/testdata/originals/$2.wav" ;;
    cover) echo "$repo/testdata/multis/$2/cover_mix.wav" ;;
    *) return 1 ;;
  esac
}

failed=0
while IFS='|' read -r kind slug src || [[ -n "$kind" ]]; do
  [[ -z "${kind// }" || "$kind" == \#* ]] && continue
  wanted "$slug" || continue
  if ! dest="$(dest_for "$kind" "$slug")"; then
    echo "skip: unknown kind '$kind' for $slug" >&2; failed=1; continue
  fi
  if [[ "$src" == http* ]]; then target="$src"; else target="ytsearch1:$src"; fi

  if [[ -f "$dest" ]]; then
    echo "have  $kind $slug -> ${dest#"$repo"/}"
    continue
  fi

  if [[ $go -eq 0 ]]; then
    if pick="$("$ytdlp" --no-warnings --skip-download --print '%(title)s | %(channel)s | %(duration_string)s | %(webpage_url)s' "$target" 2>/dev/null)"; then
      echo "pick  $kind $slug: $pick"
    else
      echo "FAIL  $kind $slug: nothing resolved for '$src'" >&2; failed=1
    fi
    continue
  fi

  mkdir -p "$(dirname "$dest")"
  # Best audio stream, decoded once to WAV (no second lossy encode); keep the source URL beside it.
  if "$ytdlp" --no-warnings --no-playlist -f bestaudio -x --audio-format wav \
      --print-to-file '%(webpage_url)s %(title)s' "${dest%.wav}.source.txt" \
      -o "${dest%.wav}.%(ext)s" "$target"; then
    echo "got   $kind $slug -> ${dest#"$repo"/}"
  else
    echo "FAIL  $kind $slug" >&2; failed=1
  fi
done < "$list"

[[ $go -eq 0 ]] && echo "Dry run. Check each pick (studio version, right song), fix the list, then re-run with --go."
exit $failed

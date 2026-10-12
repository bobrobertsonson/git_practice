#!/bin/sh
# Renders test DIs through every mode and measures them with drywet.py.
#   tools/render_all.sh                 synthetic DIs (built by synth_di)
#   tools/render_all.sh samples/        your own DIs (*.wav; "bass" in the name = bass voicing)
# Output: renders/<di>/<setting>.wav and renders/report.txt (all git-ignored).
set -e
cd "$(dirname "$0")/.."
make -s axe_wav synth_di
SRC=${1:-renders/di}
if [ -z "$1" ]; then mkdir -p renders/di && ./synth_di renders/di > /dev/null; fi
REPORT=renders/report.txt
: > "$REPORT"
for di in "$SRC"/*.wav; do
    base=$(basename "$di" .wav)
    case "$base" in *bass*|*Bass*|*BASS*) inst=bass ;; *) inst=guitar ;; esac
    # a WAV that ffmpeg can read but axe_wav can't (e.g. 24-bit extensible): convert to float first
    work="$di"
    out=renders/$base
    mkdir -p "$out"
    render() { name=$1; shift; ./axe_wav "$work" "$out/$name.wav" inst=$inst "$@"; }
    render "Tight - default" mode=tight
    render "Tight - hard (tight 1 punch 0.2)" mode=tight tight=1 punch=0.2
    render "Sub - poly" mode=sub
    render "Sub - poly sub 1 dry 0.7 grit 0.4" mode=sub sub=1 dry=0.7 grit=0.4
    render "Sub - mono (WSOLA)" mode=sub subtype=mono
    render "New Strings - default" mode=strings
    render "New Strings - bright 1 clarity 1 air 6" mode=strings bright=1 clarity=1 air=6
    render "Buzzsaw - default" mode=buzzsaw
    render "Buzzsaw - gain 1 lows 1 highs 1" mode=buzzsaw gain=1 lows=1 highs=1
    if [ "$inst" = bass ]; then render "Buzzsaw - blend 0.35" mode=buzzsaw blend=0.35; fi
    python3 tools/drywet.py --dry "$di" "$out"/*.wav >> "$REPORT"
done
# Learn: teach fresh strings on one riff, match dull strings on another
if [ -z "$1" ]; then
    ./axe_wav renders/di/guitar_dull_riff2.wav "renders/guitar_dull_riff2/New Strings - learned from guitar_fresh.wav" \
        mode=strings learnfresh=renders/di/guitar_fresh.wav bright=0.3 clarity=0.3 air=0 2>> "$REPORT"
fi
echo "wrote $REPORT"

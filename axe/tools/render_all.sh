#!/bin/sh
# Renders test DIs through every mode (fx modes with Trigger held 2-5 s) and
# measures them with drywet.py.
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
    render "Sub - mono (WSOLA)" mode=sub subtype=mono
    render "Buzzsaw - default" mode=buzzsaw
    if [ "$inst" = bass ]; then render "Buzzsaw - blend 0.35" mode=buzzsaw blend=0.35; fi
    # fx: Trigger held from 2 s to 5 s (tempo 120, 1/16 unless noted)
    T="trigat=2,5 tempo=120"
    render "Glitch - auto and held" mode=glitch $T
    render "Glitch - clean repeat 1-8" mode=glitch chaos=0 rate=1/8 $T
    render "Laser - picks, machine gun held" mode=laser $T
    render "Laser - tracking, long sweep" mode=laser ltrack=1 sweep=0.8 drop=0.3 $T
    render "Dive Bomb - pick dives, held dive" mode=dive $T
    render "Dive Bomb - 2 octaves, 2 beats" mode=dive depth=24 dtime=0.83 engage=trigger $T
    render "Chiptune - arp held" mode=chip $T
    render "Stutter Gate - trance, roll held" mode=stutter pattern=2 $T
    render "Ring Shredder - shifter, jumps held" mode=ring rtype=1 $T
    render "Ring Shredder - tracking ring" mode=ring rtrack=1 $T
    render "Freeze - held" mode=freeze engage=trigger $T
    render "Freeze - every pick" mode=freeze $T
    render "Feedback - octave held" mode=feedback engage=trigger $T
    render "Swarm - burst held" mode=swarm $T
    render "Radio - spin held" mode=radio $T
    python3 tools/drywet.py --dry "$di" "$out"/*.wav >> "$REPORT"
done
echo "wrote $REPORT"

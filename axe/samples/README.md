# Your DIs go here (git-ignored)

Everything in this folder except this README stays on your disk. `.gitignore`
also blocks audio files (wav, aiff, flac, mp3, ...) everywhere in the repo,
so session stems (Nail the Mix etc.) and renders never reach GitHub.

Useful takes: chords, palm-muted riffs, single notes (with bends and held
notes, for Chiptune, Feedback and Laser tracking), a low open string, bass
lines. Put "bass" in a bass DI's file name to get the bass voicing.

    tools/render_all.sh samples/     # renders/<di>/<mode>.wav + renders/report.txt

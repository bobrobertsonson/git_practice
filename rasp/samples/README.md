# Vocal samples

Put recordings here so `tools/takes.py` can process them.

| Folder | What goes in it |
|---|---|
| `raw/` | Dry, unprocessed takes (DI/mic straight from the interface): screams, gutturals, highs, shouts. Takes recorded with band bleed are useful for testing the live side |
| `ref/` | Finished vocals to match: isolated final vocal bounces work best. Full mixes work too, but the band skews the tone numbers |

You don't need to label takes by style: `classify` measures pitch clarity,
brightness and low end, then sorts each take into guttural, mid scream,
high scream/squeal, shout or clean, and suggests a starting preset.
Its thresholds were set on synthetic voices, so check them against real takes.

WAV, AIFF, FLAC and MP3 all work. 48 kHz is ideal; other rates are converted.
**Audio is never committed:** `.gitignore` excludes `raw/`, `ref/` and every
audio and zip file in the repo, so stems and mixes stay on your machine. The
tools also take paths anywhere on disk (for example `~/Music/NTM_Vocals/*.wav`),
so you don't have to copy anything here.

```sh
make -C rasp
python3 rasp/tools/takes.py classify rasp/samples/raw/*        # what style is each take?
python3 rasp/tools/takes.py render rasp/samples/raw/*          # every preset
python3 rasp/tools/takes.py analyze rasp/renders/* rasp/samples/ref/*
python3 rasp/tools/takes.py match rasp/samples/ref/final.wav rasp/renders/take__huge.wav
```

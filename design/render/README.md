# Sawblade design B2 - procedural "STOCKHOLM SYNDROME" pedal renders

All renders are built entirely procedurally in Blender (Cycles): geometry, shader nodes, tolex grain, the OLED
pixel font and the crust-stencil face artwork (numpy/Pillow: stencilled motifs, overspray, drips, xerox grain,
torn tape, mis-registered second ink, scratched-through paint). No downloaded models, textures or HDRIs.

| script              | piece                                            | art motif / inks                          | outputs (`--out`)                                  |
|---------------------|--------------------------------------------------|-------------------------------------------|----------------------------------------------------|
| `pedal_b2.py`       | STOCKHOLM SYNDROME pedal, 120x190x55 mm          | saw blade; orange + bone                  | `stockholm_hero_3q.png` 1600x1200, `stockholm_ortho.png` 1200x1800, `stockholm_knob_strip.png` 128x8192 |
| `pedal_tighten.py`  | TIGHTEN boost pedal, 100x150x42 mm               | vise jaws + barbed wire; steel blue + bone| `tighten_hero_3q.png`, `tighten_ortho.png` 1000x1500 |
| `pedal_fuzz.py`     | BOG BURIAL doom/sludge fuzz pedal, 120x150x50 mm (Big-Muff-style layout) | melting skull sinking into a bog + swamp sigils; acid green + rust on olive powder coat | `fuzz_hero_3q.png` 1600x1200, `fuzz_ortho.png` 1200x1500, `fuzz_knob_strip.png` 128x8192 |
| `pedal_vermin.py`   | VERMIN rat-style distortion pedal, 110x150x48 mm (three knobs DIST / FILTER / VOLUME, footswitch, LED) | spray-painted stencil rat silhouette (bridges, overspray, chips, long curling tail) + gnaw marks + drain slots; sulphur yellow + bone on gunmetal powder coat | `vermin_hero_3q.png` 1600x1200, `vermin_ortho.png` 1100x1500, `vermin_knob_strip.png` 128x8192 |
| `playalong_deck.py` | PLAY ALONG deck: battered 4-track cassette portastudio, 340x262 mm sloped top (10.5 deg), cassette well + cassette, piano-key transport, LOOP A/B, COUNT-IN, BACKING fader, GHOST/MUTE toggle, OLED counter, 5 stickers | reel-eyed tape skull + unspooling tape; riso pink + bone, plus full-colour crust stickers | `deck_hero_3q.png` 1600x1200, `deck_ortho.png` 1800x1400 (camera along the panel normal) |
| `board_shot.py`     | Hero scene: road-worn black plywood pedalboard (chipped edges, gaffer tape, zip-tied right-angle-plug patch cables, PSU brick) carrying the PLAY ALONG deck, TIGHTEN, STOCKHOLM SYNDROME and BOG BURIAL on a dark stage floor; warm key / cool rim lights, subtle DoF | all four pieces via their `build()` | `board_hero.png` 2400x1200 (`--scale 40` preview; `--az/--el/--dist/--lens/--fstop` camera) |
| `amp_saw.py`        | SAW amp head, 600x260x250 mm                     | jawbone teeth + cogs; rust orange + bone  | `saw_hero_3q.png`, `saw_ortho.png` 1600x700 (front panel) |
| `amp_body.py`       | BODY amp head, same construction                 | anvil + hammer + chains; blood red + bone | `body_hero_3q.png`, `body_ortho.png` 1600x700 |
| `cab_4x12.py`       | SAWBLADE 4x12 cab (760x760x360 mm, generic)      | howling wolf + crescent moon painted on woven grille cloth; bone + toxic green | `cab_hero_3q.png` 1600x1200, `cab_front_ortho.png` 1400x1400 (grille on), `cab_open_ortho.png` 1400x1400 (grille off, 4 drivers + mic) |
| `cab_2x12.py`       | SAWBLADE 2x12 open-back cab (740x520x290 mm, generic) for the "studio split" layout | **placeholder art, pending the user's call:** crossed bones + crust spikes + "CRUST" stencil painted on the grille cloth; bone + bruise purple | `cab2x12_hero_3q.png` 1600x1200, `cab2x12_front_ortho.png` 1400x1000 (grille on), `cab2x12_open_ortho.png` 1400x1000 (grille off, 2 drivers + mic) |
| `ui_sprites.py`     | UI control filmstrips (real sprites, transparent) | none (parts only)                         | see "UI sprites" below                              |
| `app_icon.py`       | app icon: 16-tooth saw blade (the `pedal_b2.py` motif) in the macOS rounded square | orange + bone on near-black | `plugin/assets/icon/icon_{16,32,64,128,256,512,1024}.png` (Pillow + numpy, no Blender; `--check` compares with the committed files) |

Heads and the cab share real-cab detail parts in `common.py`: tolex, metal corner caps, piping, stitched seams, strap handles; the cab adds grille cloth, generic drivers and a generic unbranded mic.

`common.py` holds the shared toolkit (materials, knob/toggle/footswitch/LED/OLED/jewel parts, artwork generator,
lighting/camera rig and the `run(spec)` driver); each piece script only defines its layout, motif and spec.

## Importable `build()` (pedals and deck)

`pedal_b2.py`, `pedal_tighten.py`, `pedal_fuzz.py`, `pedal_vermin.py` and `playalong_deck.py` each expose
`build(mats=None, origin=(0,0,0), rot_z=0.0, mode='hero', ...)` and return a root empty (everything parented to it; `origin` in mm at the
footprint centre on the base, `rot_z` in degrees). `build()` never touches camera, lights, world or render settings, so several pieces can
share one scene (see `board_shot.py`). It calls `set_piece_dims` itself and makes its powder material afterwards, because the part
builders read the shared `W_/L_/H_/Z` globals. `mats` comes from `common.make_standard_mats(...)` (pass the piece's LED colours); the
face art is generated and cached in `mats`, or passed as `art_image`. The scripts' CLI output is unchanged.

## UI sprites (`ui_sprites.py`)

Real UI assets for the JUCE build: top-down orthographic, transparent background (straight RGBA), 2x supersampled and
downsampled with premultiplied alpha, each with a JSON sidecar (`frame_width/height`, `frames`, `layout: vertical`,
`angle_min_deg/angle_max_deg`, `mm_per_px`, `pivot_px`). Parts come from `common.py` (the knurled `Knob`, `build_footswitch`,
`build_led`, `build_toggle`) plus a generic chicken-head pointer knob defined in the script. Pedal parts use the pedal ortho
camera and light rig; the amp knob uses the amp-head front-panel orientation, camera and light rig, so highlights match the
existing `*_ortho.png` renders. Centre of every frame is the part's pivot.

| `--part`                              | output (`--out`)                         | frames                                                         |
|---------------------------------------|------------------------------------------|----------------------------------------------------------------|
| `knob_pedal` (knurled, 40 mm frame)   | `knob_pedal.png` 128x16384 + `.json`     | 128 x 128x128, -135..+135 deg clockwise, pointer up = 0 deg    |
| `knob_amp` (chicken-head, 64 mm frame)| `knob_amp.png` 128x16384 + `.json`       | 128 x 128x128, same angle range                                |
| `footswitch`                          | `footswitch.png` 128x256 + `.json`       | up, down (cap 3.6 mm lower; down frame adds an overhanging collar lip, a soft shadow ring on the cap and a darker cap, so it reads from straight above) |
| `led_orange` / `led_bone` / `led_green`| `led_<colour>.png` 64x128 + `.json`     | off (dark unlit lens, ~5% of the lit colour, no emission), on; lens only, **no halo** (rendered with the 'Standard' view transform so the saturated emission does not clip to white; bone is a warm ivory-amber) |
| (written with each LED)               | `led_<colour>_glow.png` 128x256 + `.json`| glow layer: off = fully transparent, on = soft halo. 128x128 px per frame = 40 mm, i.e. the **same mm/px as the LED sprite** (0.3125) but a larger frame; align frame centres. Straight RGBA, constant colour, alpha = intensity: draw it under the LED sprite (normal blend) or additively with rgb*alpha. Computed in numpy (blurred lens disc), not rendered |
| `toggle`                              | `toggle.png` 64x192 + `.json`            | lever toward top, centre, toward bottom                        |

`--part all` renders everything; `--scale 40` is a preview, `--samples N` (default 16 at 2x supersample), `--frames N` for
rotary parts, `--ring` bakes the `common.py` LED value ring into the rotary frames (off by default: the UI draws its own
value arc; the ring radius is in the JSON). Frame for a value v in 0..1 is `round(v * (frames - 1))`; frame 0 is -135 deg and the last
frame +135 deg. No cast shadow is baked in (the UI draws its own drop shadow). Footswitch, LED and toggle sprites get a top-down chrome environment (invisible softbox strips ringed around and above the part, built in `ui_sprites.py`) so chrome shows crisp light/dark bands; the strips are camera- and shadow-invisible, so they never appear in the frame or alpha. The `cab_2x12.py` (50 mm) and `cab_4x12.py` (60 mm) ortho/open views lift the cab (and the camera with it) so the frame stays above the floor backdrop, which fixes the black band that used to run along the bottom edge.

## Run

Tested with the `bpy` wheel **4.2.0** on Python 3.11 (CPU, 3 threads, OpenImageDenoise; the sprite renders in `ui_sprites.py` use `SPRITE_THREADS = 2`).

    python3.11 -m venv /path/outside/repo/venv
    /path/outside/repo/venv/bin/pip install bpy==4.2.0 numpy pillow
    /path/outside/repo/venv/bin/python design/render/amp_saw.py --mode all --out /path/outside/repo/out

`--mode hero|ortho|strip|open|all` (strip only for `pedal_b2.py`, `pedal_fuzz.py` and `pedal_vermin.py`, open only for the cab scripts; `playalong_deck.py` takes hero|ortho|all; `board_shot.py` renders the hero only and takes no `--mode`), `--scale 40` renders a 40% preview,
`--samples N` overrides sampling (default 40), `--dump-art` writes only the artwork channel PNG,
`--font-dir` sets the font cache (default `~/.cache/pedal_b2_fonts`).

**Renders go outside the repo** (`--out`); do not commit PNGs here.

## Third-party font (fetched at run time, not committed)

The stencil lettering uses **Black Ops One** by Wojciech Kalinowski, licensed under the
SIL Open Font License 1.1:
https://raw.githubusercontent.com/google/fonts/main/ofl/blackopsone/BlackOpsOne-Regular.ttf
(licence: https://github.com/google/fonts/tree/main/ofl/blackopsone).
The script downloads it into `--font-dir` on first run; the `.ttf` must not be committed.
If the download fails it falls back to Pillow's default bitmap font.

## Exporting the plugin UI assets

`export_ui_assets.py` is the driver that renders the set the JUCE editor needs (amp heads, cab, both pedals,
knob / footswitch / LED filmstrips) with the piece scripts above and writes the post-processed results into
`plugin/assets/`. **`plugin/assets/` is the one place UI renders are committed**; no CMake step renders anything.
Usage and the exact command line used are in `plugin/assets/README.md`.

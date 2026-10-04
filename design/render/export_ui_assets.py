#!/usr/bin/env python3.11
"""Render the plugin UI asset set and post-process it into plugin/assets/.

Run with the interpreter of a venv that has bpy==4.2.0, numpy and pillow (see README.md):

    nice -n 19 /path/to/venv/bin/python design/render/export_ui_assets.py \
        --scale 48 --samples 24 --work-dir /path/outside/repo/work

The piece scripts are invoked as subprocesses (one Blender scene each) into --work-dir, then
Pillow downsamples (Lanczos, premultiplied alpha) to the stored sizes below. plugin/assets/ is the
one place UI renders are committed. CMake never renders anything; it only embeds that folder.
"""
import argparse, json, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))

# (output name, piece script, args, rendered file, stored width in px)
PIECES = [
    ('amp_saw.png',    'amp_saw.py',       ['--mode', 'ortho'], 'saw_ortho.png',        660),
    ('amp_body.png',   'amp_body.py',      ['--mode', 'ortho'], 'body_ortho.png',       660),
    ('cab_4x12.png',   'cab_4x12.py',      ['--mode', 'ortho'], 'cab_front_ortho.png',  660),
    ('pedal_saw.png',  'pedal_b2.py',      ['--mode', 'ortho'], 'stockholm_ortho.png',  360),
    ('pedal_body.png', 'pedal_tighten.py', ['--mode', 'ortho'], 'tighten_ortho.png',    280),
]
# (part, output stem)
SPRITES = [('knob_amp', 'knob_amp'), ('knob_pedal', 'knob_pedal'), ('footswitch', 'footswitch'), ('led_orange', 'led_orange')]

# every file the editor embeds (CMake lists these explicitly; a test checks they exist)
def asset_files():
    files = [p[0] for p in PIECES]
    for _, stem in SPRITES:
        files += [stem + '.png', stem + '.json']
    return files


def downscale(src, dst, width):
    """Lanczos with premultiplied alpha (no dark fringes on transparent edges)."""
    from PIL import Image
    im = Image.open(src).convert('RGBA')
    if im.width < width:
        print('WARNING: %s is %d px wide, below the stored width %d; rerun with a higher --scale' % (src, im.width, width))
    h = round(im.height * width / im.width)
    out = im.convert('RGBa').resize((width, h), Image.LANCZOS).convert('RGBA')
    out.save(dst, optimize=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(REPO, 'plugin', 'assets'))
    ap.add_argument('--python', default=sys.executable, help='interpreter for the piece scripts (bpy 4.2.0 venv)')
    ap.add_argument('--scale', type=int, default=100, help='render resolution percentage for the ortho renders')
    ap.add_argument('--samples', type=int, default=0, help='Cycles samples for the ortho renders (0 = script default)')
    ap.add_argument('--sprite-samples', type=int, default=16)
    ap.add_argument('--list', action='store_true', help='print the asset file names the editor embeds and exit')
    ap.add_argument('--work-dir', help='scratch directory (outside the repo)')
    ap.add_argument('--skip-existing', action='store_true', help='reuse renders already in --work-dir')
    a = ap.parse_args()
    if a.list:
        print('\n'.join(asset_files()))
        return
    if not a.work_dir:
        ap.error('--work-dir is required')
    work = os.path.abspath(a.work_dir)
    if os.path.commonpath([work, REPO]) == REPO:
        sys.exit('--work-dir must be outside the repo')
    os.makedirs(work, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    t0 = time.time()

    def run(cmd):
        print('+', ' '.join(cmd), flush=True)
        subprocess.run(cmd, check=True)

    for name, script, extra, rendered, width in PIECES:
        src = os.path.join(work, rendered)
        if not (a.skip_existing and os.path.exists(src)):
            cmd = [a.python, os.path.join(HERE, script)] + extra + ['--out', work, '--scale', str(a.scale)]
            if a.samples:
                cmd += ['--samples', str(a.samples)]
            run(cmd)
        downscale(src, os.path.join(a.out, name), width)
    for part, stem in SPRITES:
        if not (a.skip_existing and os.path.exists(os.path.join(work, stem + '.png'))):
            run([a.python, os.path.join(HERE, 'ui_sprites.py'), '--part', part, '--out', work,
                 '--samples', str(a.sprite_samples)])
        shutil.copyfile(os.path.join(work, stem + '.png'), os.path.join(a.out, stem + '.png'))
        shutil.copyfile(os.path.join(work, stem + '.json'), os.path.join(a.out, stem + '.json'))
    total = sum(os.path.getsize(os.path.join(a.out, f)) for f in asset_files())
    print('done in %.0fs; %d files, %.2f MB in %s' % (time.time() - t0, len(asset_files()), total / 1e6, a.out))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Render every workshop mockup screen into design/mockups/workshop/png/.

    python3 design/mockups/workshop/render_all.py --look v3 --wear strong   # one set (look v2|v3|all x wear subtle|strong|all)
    python3 design/mockups/workshop/render_all.py                 # render all four sets (png, png_strong, png_v3, png_v3_strong)
    python3 design/mockups/workshop/render_all.py --wear strong   # only the strong set
    python3 design/mockups/workshop/render_all.py --only 01_main_rig
    python3 design/mockups/workshop/render_all.py --check         # re-render to a temp dir, compare decoded RGBA with png/
    python3 design/mockups/workshop/render_all.py --contrast      # print the measured contrast table (Markdown), no files

Exit codes: 0 ok; 1 a PNG differs (--check) or a text pair is below WCAG 2.x AA (any mode); 77 Pillow / numpy missing or a
font could not be fetched / verified (never a silent fallback).

Adding a screen: create ``screens_<topic>.py`` exposing ``SCREENS = {'NN_name': fn}`` (``fn()`` returns a
``workshop_style.Canvas``) and add the module name to ``SCREEN_MODULES`` below (a fixed, sorted list: no directory scanning,
so the render order never depends on the filesystem).  ``00_style_sheet`` comes from ``workshop_style.render_style_sheet``.
"""
import argparse
import importlib
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'render')))

# fixed, sorted.  Task A step 2 adds screens_bar (02), screens_settings (03, 04), screens_match (05, 06), screens_woodshed (07),
# screens_editor (08), screens_notices (09).
SCREEN_MODULES = ['screens_panels', 'screens_rig', 'screens_tools']

PNG_DIR = os.path.join(HERE, 'png')


def load_registry():
    """-> (ordered {name: fn}); name -> module for error messages."""
    import workshop_style as ws
    reg = {'00_style_sheet': ws.render_style_sheet}
    for m in SCREEN_MODULES:
        mod = importlib.import_module(m)
        for name, fn in mod.SCREENS.items():
            if name in reg:
                raise SystemExit(f'duplicate screen name {name} (module {m})')
            reg[name] = fn
    return dict(sorted(reg.items()))


OVERPAINT = []          # (screen, string, delta, box): strings painted over after they were drawn (a failure)


# v4 renders only these screens (plus the three cab directions of 01); v2 / v3 render the ten screens and skip the cab variants
V4_SCREENS = ('01_main_rig', '01_main_rig_cab_a', '01_main_rig_cab_b', '01_main_rig_cab_c', '02_top_bar', '03_settings_calibration',
              '08_rig_editor')
CAB_VARIANTS = ('01_main_rig_cab_a', '01_main_rig_cab_b', '01_main_rig_cab_c')


def screens_for(reg, look):
    if look == 'v4':
        return {k: v for k, v in reg.items() if k in V4_SCREENS}
    return {k: v for k, v in reg.items() if k not in CAB_VARIANTS}


def render_screens(reg, only=None):
    """-> {name: (PIL image 1x, log list)}"""
    out = {}
    for name, fn in reg.items():
        if only and name not in only:
            continue
        cv = fn()
        out[name] = (cv.finish(), cv.log)
        for text_, delta, box in cv.overpaint:
            OVERPAINT.append((name, text_, delta, box))
    return out


def contrast_table(results):
    """-> (markdown lines, failures list).  One row per (style, fg, bg)."""
    import workshop_style as ws
    groups = {}
    fails = []
    for name, (_img, log) in results.items():
        for e in log:
            bg = ws.token_name(e['bg_rgb'])
            key = (e['style'], e['fg'] if isinstance(e['fg'], str) else ws.token_name(e['fg_rgb']), bg)
            g = groups.setdefault(key, dict(ratio=1e9, sizes=set(), uses=0, screens=set(), need=0.0, bold=False))
            g['ratio'] = min(g['ratio'], e['ratio'])
            g['sizes'].add(e['px'])
            g['uses'] += 1
            g['screens'].add(name[:2])
            need = ws.required_ratio(e['px'], e['bold'])
            g['need'] = max(g['need'], need)
            g['bold'] = g['bold'] or e['bold']
            if e['ratio'] < need - 1e-9:
                fails.append((name, e['text'], key, e['ratio'], need))
    lines = ['| style | fg | bg | ratio | AA needs | px sizes | uses | screens |',
             '|---|---|---|---|---|---|---|---|']
    for key in sorted(groups):
        g = groups[key]
        sizes = ','.join(('%g' % s) for s in sorted(g['sizes']))
        ok = 'ok' if g['ratio'] >= g['need'] - 1e-9 else 'FAIL'
        lines.append('| %s | %s | %s | %.2f:1 %s | %.1f | %s | %d | %s |' % (
            key[0], key[1], key[2], g['ratio'], ok, g['need'], sizes, g['uses'], ' '.join(sorted(g['screens']))))
    return lines, fails


def set_dirs(out, looks, wears):
    """-> [(look, wear, directory)].  Default: png/ (v2 subtle), png_strong/, png_v3/, png_v3_strong/ next to this file; with
    --out DIR the same names are made from DIR (DIR, DIR_strong, DIR_v3, DIR_v3_strong), and a single selected set goes to DIR."""
    res = []
    n = len(looks) * len(wears)
    for lk in looks:
        for w in wears:
            suffix = ('_' + lk if lk in ('v3', 'v4') else '') + ('_strong' if w == 'strong' else '')
            base = PNG_DIR if out is None else os.path.normpath(out)
            res.append((lk, w, base if (out is not None and n == 1) else base + suffix))
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=None, help='output directory (default png/ for subtle, png_strong/ for strong)')
    ap.add_argument('--font-dir', default=None, help='font cache directory (default ~/.cache/sawblade_fonts)')
    ap.add_argument('--wear', default='all', choices=['subtle', 'strong', 'all'], help='material wear set (default: both)')
    ap.add_argument('--look', default='all', choices=['v2', 'v3', 'v4', 'all'], help='v2 (approved look), v3 (displays) or v4 (streamlined) (default: all)')
    ap.add_argument('--check', action='store_true', help='compare a fresh render with the committed PNGs')
    ap.add_argument('--contrast', action='store_true', help='print the measured contrast table (Markdown)')
    ap.add_argument('--only', action='append', help='render only this screen name (repeatable)')
    a = ap.parse_args(argv)
    try:
        import numpy as np
        from PIL import Image
        import workshop_style as ws
        ws.ensure_fonts(a.font_dir) if a.font_dir else ws.ensure_fonts()
    except ImportError as e:
        print(f'render_all.py: SKIP, needs Pillow and numpy ({e})')
        return 77
    except ws.FontError as e:
        print(f'render_all.py: SKIP, {e}', file=sys.stderr)
        return 77
    reg = load_registry()
    wears = ['subtle', 'strong'] if a.wear == 'all' else [a.wear]
    looks = ['v2', 'v3', 'v4'] if a.look == 'all' else [a.look]
    rc = 0
    for look, wear, outdir in set_dirs(a.out, looks, wears):
        ws.set_wear(wear)
        ws.set_look(look)
        del OVERPAINT[:]
        results = render_screens(screens_for(reg, look), a.only)
        for o in OVERPAINT:
            print(f'[{look} {wear}] OVERPAINT FAIL {o[0]}: {o[1]!r} changed by {o[2]}/255 at {o[3]}')
        if OVERPAINT:
            rc |= 1
        lines, fails = contrast_table(results)
        tag = f'[{look} {wear}] '
        if a.contrast:
            if len(wears) * len(looks) > 1:
                print(f'### look: {look}, wear: {wear}\n')
            print('\n'.join(lines))
            n = sum(len(v[1]) for v in results.values())
            print(f'\n{n} strings measured on {len(results)} screens: ' + ('all pass WCAG 2.x AA' if not fails else f'{len(fails)} FAIL'))
            for f in fails:
                print('FAIL', f)
            rc |= 1 if fails else 0
            continue
        if a.check:
            bad = 0
            for name, (img, _log) in results.items():
                path = os.path.join(outdir, name + '.png')
                if not os.path.exists(path):
                    print(f'{tag}MISSING {path}')
                    bad += 1
                    continue
                old = np.asarray(Image.open(path).convert('RGBA'))
                new = np.asarray(img.convert('RGBA'))
                if old.shape != new.shape or not np.array_equal(old, new):
                    print(f'{tag}DIFFERS {name}')
                    bad += 1
            bad += len(fails)
            for f in fails:
                print(f'{tag}CONTRAST FAIL', f)
            print(f'{tag}workshop mockups: ' + (f'all {len(results)} PNGs match a fresh render' if bad == 0 else f'{bad} problem(s)'))
            rc |= 1 if bad else 0
            continue
        os.makedirs(outdir, exist_ok=True)
        for name, (img, _log) in results.items():
            path = os.path.join(outdir, name + '.png')
            ws.save_png(img, path)
            print('%s%-24s %dx%d  %7.1f KB' % (tag, name, img.width, img.height, os.path.getsize(path) / 1024.0))
        for f in fails:
            print(f'{tag}CONTRAST FAIL', f)
        rc |= 1 if fails else 0
    ws.set_wear('subtle')
    ws.set_look('v2')
    return rc


if __name__ == '__main__':
    sys.exit(main())

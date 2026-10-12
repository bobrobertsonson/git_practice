#!/usr/bin/env python3
"""Phase 7b report plots from the pedal_fr_report.sh CSVs (python3 tests/tools/pedal_fr_plots.py --csv-dir DIR --out docs/reports/phase7b).

Inputs (in --csv-dir):
  preset_<stem>.csv            freq_hz,mag_db   (one per preset, pedal settings of the preset)
  clip_<type>_thd.csv          input_dbfs,thd_db,h2_dbc
  mode_<mode>.csv              freq_hz,mag_db   (low=high=dist=10)
Outputs PNGs into --out.
"""
import argparse, csv, glob, os
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# dataviz reference palette, light mode (fixed categorical order)
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#4a3aa7", "#e34948"]
TEXT, TEXT2, GRID, SURF = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"


def read_csv(path):
    with open(path) as f:
        r = csv.reader(f)
        hdr = next(r)
        rows = np.array([[float(x) for x in row] for row in r])
    return hdr, rows


def style(ax, xlabel, ylabel, title):
    ax.set_facecolor(SURF)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.tick_params(colors=TEXT2, labelsize=9)
    ax.set_xlabel(xlabel, color=TEXT2, fontsize=10)
    ax.set_ylabel(ylabel, color=TEXT2, fontsize=10)
    ax.set_title(title, color=TEXT, fontsize=12, loc="left", pad=10)


def fr_series(path):
    _, rows = read_csv(path)
    f, m = rows[:, 0], rows[:, 1]
    sel = (f >= 20) & (f <= 20000)
    f, m = f[sel], m[sel]
    ref = np.interp(400.0, f, m)
    return f, m - ref


def plot_presets(csv_dir, out, names):
    order = [["classic_buzzsaw", "early_raw_demo", "dbeat_crust", "powerviolence_hardcore"],
             ["grind", "death_n_roll", "modern_tight_swedish", "blend_partner"],
             ["custom_wall", "modded_nasty", "bass_chainsaw", "clean_mix_texture"],
             ["pickle_chainsaw", "pickle_doom_saw", "pickle_into_saw"]]
    titles = ["Chainsaw circuit presets (1 of 3)", "Chainsaw circuit presets (2 of 3)",
              "Chainsaw circuit presets (3 of 3)", "Big Fuzz circuit presets (first block)"]
    fig, axes = plt.subplots(len(order), 1, figsize=(9, 3.6 * len(order)), facecolor=SURF)
    for ax, grp, title in zip(axes, order, titles):
        for k, stem in enumerate(grp):
            p = os.path.join(csv_dir, "preset_%s.csv" % stem)
            if not os.path.exists(p):
                continue
            f, m = fr_series(p)
            ax.semilogx(f, m, color=SERIES[k], linewidth=2, label=names.get(stem, stem))
        style(ax, "frequency (Hz)", "dB re 400 Hz", title + ": small-signal response")
        ax.set_xlim(20, 20000)
        ax.set_ylim(-60, 30)
        ax.legend(frameon=False, fontsize=8, labelcolor=TEXT, loc="lower left")
    fig.tight_layout()
    fig.savefig(os.path.join(out, "presets_fr.png"), dpi=130)
    plt.close(fig)


def plot_thd(csv_dir, out):
    fig, axes = plt.subplots(1, 2, figsize=(10, 4), facecolor=SURF)
    for ax, circ, label in zip(axes, ["hm", "muff"], ["Chainsaw", "Big Fuzz"]):
        for k, clip in enumerate(["silicon", "led", "asymmetric", "soft"]):
            p = os.path.join(csv_dir, "clip_%s_%s_thd.csv" % (circ, clip))
            if not os.path.exists(p):
                continue
            _, rows = read_csv(p)
            ax.plot(rows[:, 0], rows[:, 1], color=SERIES[k], linewidth=2, marker="o", markersize=4, label=clip)
        style(ax, "input level (dBFS)", "THD (dB re fundamental)", "%s: THD vs input per clip (500 Hz, drive 5)" % label)
        ax.set_ylim(-40, 0)
        ax.legend(frameon=False, fontsize=9, labelcolor=TEXT, loc="lower right")
    fig.tight_layout()
    fig.savefig(os.path.join(out, "clip_thd.png"), dpi=130)
    plt.close(fig)


def plot_muff(csv_dir, out):
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 4), facecolor=SURF)
    for k, t in enumerate(["0", "5", "10"]):
        p = os.path.join(csv_dir, "muff_tone%s.csv" % t)
        if os.path.exists(p):
            f, m = fr_series(p)
            a1.semilogx(f, m, color=SERIES[k], linewidth=2, label="tone %s" % t)
    for k, sc in enumerate(["0", "10"]):
        p = os.path.join(csv_dir, "muff_scoop%s.csv" % sc)
        if os.path.exists(p):
            f, m = fr_series(p)
            a2.semilogx(f, m, color=SERIES[k], linewidth=2, label="scoop %s" % sc)
    style(a1, "frequency (Hz)", "dB re 400 Hz", "Big Fuzz: tone control")
    style(a2, "frequency (Hz)", "dB re 400 Hz", "Big Fuzz: scoop at tone 5")
    for ax in (a1, a2):
        ax.set_xlim(20, 20000)
        ax.legend(frameon=False, fontsize=9, labelcolor=TEXT, loc="lower left")
    fig.tight_layout()
    fig.savefig(os.path.join(out, "muff_tone_scoop.png"), dpi=130)
    plt.close(fig)


def plot_modes(csv_dir, out):
    files = sorted(glob.glob(os.path.join(csv_dir, "mode_*.csv")))
    if not files:
        return
    fig, ax = plt.subplots(figsize=(9, 4), facecolor=SURF)
    for k, p in enumerate(files):
        name = os.path.basename(p)[len("mode_"):-4]
        f, m = fr_series(p)
        ax.semilogx(f, m, color=SERIES[k], linewidth=2, label=name)
    style(ax, "frequency (Hz)", "dB re 400 Hz", "Mode switch at low = high = dist = 10")
    ax.set_xlim(20, 20000)
    ax.legend(frameon=False, fontsize=9, labelcolor=TEXT)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "modes_fr.png"), dpi=130)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv-dir", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    names = {"classic_buzzsaw": "Classic Buzzsaw", "early_raw_demo": "Early Raw Demo", "dbeat_crust": "D-Beat Crust",
             "powerviolence_hardcore": "Powerviolence Hardcore", "grind": "Grind", "death_n_roll": "Death 'n' Roll",
             "modern_tight_swedish": "Modern Tight Swedish", "blend_partner": "Blend Partner", "custom_wall": "Custom Wall",
             "modded_nasty": "Modded Nasty", "bass_chainsaw": "Bass Chainsaw", "clean_mix_texture": "Clean Mix Texture",
             "pickle_chainsaw": "Big Fuzz Chainsaw", "pickle_doom_saw": "Big Fuzz Doom Saw", "pickle_into_saw": "Fuzz Into Saw"}
    plot_presets(a.csv_dir, a.out, names)
    plot_thd(a.csv_dir, a.out)
    plot_modes(a.csv_dir, a.out)
    plot_muff(a.csv_dir, a.out)
    print("plots written to", a.out)


if __name__ == "__main__":
    main()

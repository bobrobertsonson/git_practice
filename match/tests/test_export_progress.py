"""``--progress-json`` writer and the pure helpers behind it (no sawblade_core, no trainer needed)."""
from __future__ import annotations

import json

import pytest

from sawblade_match.export import progress as PG

KEYS = {"stage", "fraction", "etaSeconds", "epoch", "epochs", "bestEsr", "message", "outDir", "resumable",
        "elapsedSeconds"}


class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


def read(p):
    return json.loads(p.read_text())


def test_no_path_writes_nothing(tmp_path):
    p = PG.Progress(None)
    p.update("train", 0.5, epoch=1)
    p.update("done")
    assert list(tmp_path.iterdir()) == []


def test_shape_and_atomic_file(tmp_path):
    f = tmp_path / "p.json"
    p = PG.Progress(f, out_dir=tmp_path / "out", clock=Clock())
    p.update("plan")
    d = read(f)
    assert set(d) == KEYS and d["stage"] == "plan" and d["etaSeconds"] == -1 and d["bestEsr"] is None
    assert d["outDir"] == str(tmp_path / "out") and d["resumable"] is False
    assert [x.name for x in tmp_path.iterdir()] == ["p.json"]               # no temp file left behind


def test_stage_changes_and_final_write_are_immediate_but_batches_are_throttled(tmp_path):
    f = tmp_path / "p.json"
    c = Clock()
    p = PG.Progress(f, clock=c)
    p.update("train", PG.stage_fraction("train", 0.0), epochs=10)
    assert read(f)["fraction"] == pytest.approx(0.10)
    for i in range(1, 6):                                                    # five batches within one second
        c.t += 0.1
        p.update("train", PG.stage_fraction("train", i / 100), epoch=0)
    assert read(f)["fraction"] == pytest.approx(0.10)                        # throttled: file unchanged
    c.t += 1.0
    p.update("train", PG.stage_fraction("train", 0.2))
    assert read(f)["fraction"] == pytest.approx(0.26)                        # a second later: written
    p.update("train", PG.stage_fraction("train", 0.3), epoch=3, force=True)  # epoch end: immediate
    assert read(f)["epoch"] == 3
    p.update("validate", message="v")
    assert read(f)["stage"] == "validate" and read(f)["fraction"] == pytest.approx(0.90)
    p.update("done", out_dir="/x")
    d = read(f)
    assert d["stage"] == "done" and d["fraction"] == 1.0 and d["etaSeconds"] == 0 and d["outDir"] == "/x"


def test_fraction_is_monotonic_and_clamped_to_the_stage(tmp_path):
    f = tmp_path / "p.json"
    p = PG.Progress(f, clock=Clock())
    seen = []
    for stage, fr in [("plan", 0.5), ("signal", 0.0), ("render", 1.0), ("train", 0.0), ("train", 5.0),
                      ("train", 0.2), ("validate", 0.0), ("validate", 0.99)]:
        p.update(stage, fr)
        seen.append(p.state["fraction"])
    assert seen == sorted(seen)
    assert seen[0] == 0.02 and seen[2] == 0.10 and seen[4] == 0.90 and seen[-1] == 0.99
    p.update("error", message="boom")
    d = read(f)
    assert d["stage"] == "error" and d["message"] == "boom" and d["fraction"] == 0.99      # keeps the last fraction


def test_cancelled_keeps_fraction_and_resumable(tmp_path):
    f = tmp_path / "p.json"
    p = PG.Progress(f, clock=Clock())
    p.update("train", PG.stage_fraction("train", 0.5), epoch=2, epochs=4, resumable=True)
    p.update("cancelled", message="cancelled")
    d = read(f)
    assert d["stage"] == "cancelled" and d["resumable"] is True and d["fraction"] == pytest.approx(0.5)


def test_unknown_stage_is_rejected(tmp_path):
    with pytest.raises(ValueError):
        PG.Progress(tmp_path / "p.json").update("bogus")


def test_eta_and_train_fraction():
    assert PG.eta_seconds(0.0, 0, 0, 10, 0.0, 600.0) == -1                  # no epoch finished yet
    assert PG.eta_seconds(60.0, 2, 2, 10, 60.0, 6000.0) == 240              # 30 s/epoch x 8 left
    assert PG.eta_seconds(60.0, 2, 2, 10, 60.0, 100.0) == 40                # capped by --max-minutes
    assert PG.eta_seconds(900.0, 3, 3, 10, 900.0, 600.0) == 0               # cap already passed
    # resumed: 4 epochs came from an earlier session, this session did 1 in 30 s
    assert PG.eta_seconds(30.0, 1, 5, 10, 500.0, 6000.0) == 150
    assert PG.train_progress(2, 0.5, 10, 10.0, 600.0) == pytest.approx(0.25)
    assert PG.train_progress(1, 0.0, 10, 300.0, 600.0) == pytest.approx(0.5)  # elapsed / cap wins
    assert PG.train_progress(10, 1.0, 10, 0.0, 600.0) == 1.0


def test_heartbeat_keeps_elapsed_ticking_and_fraction_monotonic(tmp_path):
    import time
    f = tmp_path / "p.json"
    p = PG.Progress(f)
    p.update("render", message="rendering")
    seen = []
    with p.heartbeat("render", interval=0.2, tau=1.0):
        for _ in range(14):                                  # a "render" that reports nothing for ~2.8 s
            time.sleep(0.2)
            seen.append(read(f))
    assert len({d["elapsedSeconds"] for d in seen}) >= 3 and len(seen) == 14
    el = [d["elapsedSeconds"] for d in seen]
    fr = [d["fraction"] for d in seen]
    assert el == sorted(el) and el[-1] - el[0] >= 2.0
    assert fr == sorted(fr) and fr[-1] > fr[0] >= 0.05 and fr[-1] <= PG.STAGE_RANGE["render"][1]
    assert all(d["stage"] == "render" for d in seen)
    assert [x.name for x in tmp_path.iterdir()] == ["p.json"]              # atomic writes leave no temp file
    n = read(f)["elapsedSeconds"]
    time.sleep(0.5)
    assert read(f)["elapsedSeconds"] == n                                  # the thread stopped with the context


def test_eta_is_seeded_before_the_first_epoch_finishes():
    # fresh start: 25% of epoch 1 took 10 s -> 40 s/epoch x 9.75 epochs left
    assert PG.eta_seconds(10.0, 0, 0, 10, 10.0, 6000.0, batch_frac=0.25) == 390
    assert PG.eta_seconds(0.0, 0, 0, 10, 0.0, 6000.0, batch_frac=0.0) == -1     # nothing measurable yet
    assert PG.eta_seconds(10.0, 0, 0, 10, 10.0, 100.0, batch_frac=0.25) == 90   # still capped by --max-minutes
    # resumed with a checkpoint rate of 30 s/epoch: an estimate from the very first sample, partial epoch ignored
    assert PG.eta_seconds(0.0, 0, 4, 10, 120.0, 6000.0, prior_epoch_s=30.0) == 180
    assert PG.eta_seconds(2.0, 0, 4, 10, 122.0, 6000.0, batch_frac=0.5, prior_epoch_s=30.0) == 165
    # once this session finished an epoch its own mean wins over the checkpoint's
    assert PG.eta_seconds(20.0, 1, 5, 10, 140.0, 6000.0, batch_frac=0.1, prior_epoch_s=30.0) == 100


def test_prior_epoch_seconds_from_a_checkpoint():
    assert PG.prior_epoch_seconds(400.0, 2) == 200.0
    assert PG.prior_epoch_seconds(0.0, 2) is None and PG.prior_epoch_seconds(50.0, 0) is None
    # resumed at epoch 4 of 10 (120 s spent): the first sample after the resume already has an ETA
    prior = PG.prior_epoch_seconds(120.0, 4)
    assert PG.eta_seconds(0.0, 0, 4, 10, 120.0, 6000.0, prior_epoch_s=prior) == 180

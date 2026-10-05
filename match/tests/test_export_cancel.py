"""Cancel decision of the trainer callback (no torch needed: the decision lives in module-level helpers)."""
from __future__ import annotations

import types

import pytest

from sawblade_match.export import resume as R
from sawblade_match.export import stop as STOP
from sawblade_match.export import train as T


@pytest.fixture(autouse=True)
def _clean_flag():
    STOP.clear()
    yield
    STOP.clear()


def fake_run():
    return types.SimpleNamespace(interrupted=False, cancel_after_epoch=False)


def test_no_stop_requested_does_nothing():
    r = fake_run()
    T.batch_cancel(r, 10, 3)
    T.epoch_end_cancel(r, "/nonexistent")
    assert not r.interrupted and not r.cancel_after_epoch


def test_stop_on_a_non_last_batch_raises_immediately():
    r = fake_run()
    STOP.request_stop()
    with pytest.raises(T._Cancel):
        T.batch_cancel(r, 10, 3)
    assert r.interrupted and not r.cancel_after_epoch


def test_stop_on_the_last_batch_defers_the_cancel_until_the_epoch_is_checkpointed(tmp_path):
    r = fake_run()
    STOP.request_stop()
    T.batch_cancel(r, 10, 9)                                   # no raise: the epoch's training is complete
    assert r.cancel_after_epoch and not r.interrupted          # validation + checkpoint guards stay open
    # epoch end: checkpoint + progress are written by the callback, then the deferred cancel marks and raises
    (tmp_path / R.LAST).write_bytes(b"epoch-3")
    R.write_progress(tmp_path, {"epoch": 3, "complete": False})
    with pytest.raises(T._Cancel):
        T.epoch_end_cancel(r, tmp_path)
    assert r.interrupted
    prog = R.read_progress(tmp_path)
    assert prog["epoch"] == 3 and prog["interrupted"] is True and (tmp_path / R.LAST).read_bytes() == b"epoch-3"

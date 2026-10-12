"""Cooperative cancel: the CLI's SIGINT handler sets the flag, the trainer callback and the stage boundaries poll it."""
from __future__ import annotations

import threading

_STOP = threading.Event()


class ExportInterrupted(Exception):
    """The run was cancelled (SIGINT); the checkpoint of the last complete epoch is kept and resumable."""


def request_stop() -> None:
    _STOP.set()


def stop_requested() -> bool:
    return _STOP.is_set()


def clear() -> None:
    _STOP.clear()

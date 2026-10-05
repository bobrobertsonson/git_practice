"""Checkpoint download with pinned sha256; partial downloads are deleted on any failure."""
from __future__ import annotations

from pathlib import Path
from typing import Callable

import httpx

from .paths import checkpoint_path
from .store import CHECKPOINT_BASE_URL, CHECKPOINTS, sha256_file


class DownloadError(RuntimeError):
    pass


def ensure_checkpoint(model_id: str, directory: Path, *, client: httpx.Client | None = None,
                      log: Callable[[str], None] = print) -> Path:
    """Return the verified checkpoint path, downloading it if absent or hash-incorrect."""
    name, want = CHECKPOINTS[model_id]
    dest = checkpoint_path(name, directory)
    if dest.is_file():
        if sha256_file(dest) == want:
            log(f"checkpoint {name}: cached, sha256 ok")
            return dest
        log(f"checkpoint {name}: cached file has the wrong sha256; re-downloading")
        dest.unlink()
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".partial")
    url = f"{CHECKPOINT_BASE_URL}/{name}"
    own = client is None
    client = client or httpx.Client(follow_redirects=True, timeout=httpx.Timeout(60.0, read=120.0))
    log(f"downloading {url}")
    try:
        with client.stream("GET", url) as r:
            if r.status_code != 200:
                raise DownloadError(f"{url}: HTTP {r.status_code}")
            with open(part, "wb") as f:
                for chunk in r.iter_bytes(1 << 20):
                    f.write(chunk)
        got = sha256_file(part)
        if got != want:
            raise DownloadError(f"{name}: sha256 {got} != pinned {want}")
        part.replace(dest)
    except BaseException as e:
        part.unlink(missing_ok=True)
        if isinstance(e, (httpx.HTTPError, OSError)):
            raise DownloadError(f"{url}: {e}") from e
        raise
    finally:
        if own:
            client.close()
    log(f"checkpoint {name}: downloaded, sha256 ok")
    return dest

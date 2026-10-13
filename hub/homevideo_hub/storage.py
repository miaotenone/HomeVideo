from __future__ import annotations

import hashlib
import shutil
from collections.abc import AsyncIterable
from pathlib import Path

_BLOCK_SIZE = 1024 * 1024


class InsufficientStorage(Exception):
    """Raised when free disk space cannot cover an incoming upload."""


async def write_stream(stream: AsyncIterable[bytes], dest: Path) -> tuple[int, str]:
    """Stream ``stream`` into ``dest``, returning ``(size_bytes, sha256_hex)``.

    Chunks are written as they arrive and folded into the digest, so a
    snapshot is never assembled in memory. The caller is responsible for
    publishing ``dest`` atomically and for deleting it on any failure path.
    """

    digest = hashlib.sha256()
    size_bytes = 0
    with dest.open("wb") as handle:
        async for chunk in stream:
            if not chunk:
                continue
            handle.write(chunk)
            digest.update(chunk)
            size_bytes += len(chunk)
    return size_bytes, digest.hexdigest()


def sha256_file(path: Path) -> str:
    """Return the hex SHA-256 of an existing file, read in blocks."""

    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(_BLOCK_SIZE), b""):
            digest.update(block)
    return digest.hexdigest()


def safe_delete(path: Path) -> None:
    """Delete ``path`` if it exists; do nothing when it is already gone."""

    try:
        path.unlink()
    except FileNotFoundError:
        pass


def free_bytes(path: Path) -> int:
    """Return the free bytes on the filesystem backing ``path``."""

    return shutil.disk_usage(path).free


def ensure_capacity(
    path: Path, required_bytes: int, reserve_bytes: int
) -> None:
    """Raise :class:`InsufficientStorage` unless ``required_bytes`` fit.

    Capacity is judged purely from the filesystem's free space: the
    upload is rejected when free space minus ``reserve_bytes`` (the head
    room kept for metadata) is smaller than the bytes to be written.
    """

    if free_bytes(path) - reserve_bytes < required_bytes:
        raise InsufficientStorage(
            f"needed {required_bytes} bytes, free space minus reserve is "
            f"insufficient at {path}"
        )

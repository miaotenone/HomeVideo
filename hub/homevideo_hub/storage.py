from __future__ import annotations

import hashlib
from collections.abc import AsyncIterable
from pathlib import Path

_BLOCK_SIZE = 1024 * 1024


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

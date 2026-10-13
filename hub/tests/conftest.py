from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from homevideo_hub.api import create_app
from homevideo_hub.config import load_settings


@pytest.fixture
def settings(tmp_path):
    return load_settings(data_dir=tmp_path)


@pytest.fixture
def client(settings):
    with TestClient(create_app(settings)) as c:
        yield c

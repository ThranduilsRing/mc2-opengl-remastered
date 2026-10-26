"""Native launches retain process ownership and provenance checks."""
import os
from pathlib import Path
import sys

import pytest

from scripts.smoke_lib import child_registry, exe_guard


def test_native_executable_uses_engine_safety_checks():
    assert exe_guard.engine_kind("/home/joe/mc2-installs/linux-port/mc2") == "game"


@pytest.mark.skipif(sys.platform != "linux", reason="Linux process metadata")
def test_native_process_identity_and_enumeration():
    pid = os.getpid()
    created = child_registry.process_create_time(pid)
    assert created is not None and created > 0
    assert child_registry.process_create_time(pid) == created
    assert child_registry.image_path(pid) == os.readlink(f"/proc/{pid}/exe")
    name = Path(f"/proc/{pid}/comm").read_text().strip()
    assert (pid, name) in child_registry.engine_processes(images={name})
    assert (pid, name) not in child_registry.engine_processes(images={name}, exclude={pid})

import sys
import os
import tempfile

# Add tools directory to sys.path
tools_path = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'tools'))
if tools_path not in sys.path:
    sys.path.insert(0, tools_path)

# Keep the harness cursor guard off the real desktop (see scripts/tests/conftest.py).
os.environ["MC2_CURSOR_GUARD_OFF"] = "1"
# A private host lease file per test process: tests that spawn a fake engine contend with each other, never with the lanes
# (scripts/smoke_lib/host_lease.py raises when a test reaches the real lease without this).
os.environ.setdefault("MC2_HOST_LEASE_PATH", os.path.join(tempfile.gettempdir(), f"mc2-host-lease-test-{os.getpid()}.json"))

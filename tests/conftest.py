import sys
import os

# Add tools directory to sys.path
tools_path = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'tools'))
if tools_path not in sys.path:
    sys.path.insert(0, tools_path)

# Keep the harness cursor guard off the real desktop (see scripts/tests/conftest.py).
os.environ["MC2_CURSOR_GUARD_OFF"] = "1"

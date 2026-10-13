#!/usr/bin/env python3
"""Compatibility entry point for protected capture drivers and release scripts.

The free-space implementation lives in deploy_payload; this path remains until
the capture lane can repoint its callers without crossing file ownership.
"""
import sys
from deploy_payload import disk_space_main


if __name__ == "__main__":
    sys.exit(disk_space_main())

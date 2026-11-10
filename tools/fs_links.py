"""fs_links.py — symlink/junction detection that os.path.islink misses on Windows.

Since Python 3.13, islink()/Path.is_symlink() report S_IFLNK only, so a Windows
junction (a reparse point, not S_IFLNK) reads as a plain directory: os.walk
(followlinks=False) and Path.rglob descend into it, and a delete or walk that
trusts islink can follow it out of its tree. The raw FILE_ATTRIBUTE_REPARSE_POINT
attribute catches it.
"""
import os
import stat

REPARSE_POINT = 0x400  # FILE_ATTRIBUTE_REPARSE_POINT


def is_link_or_junction(path) -> bool:
    """True when path is a symlink or a Windows junction/mount point.

    On Windows the reparse-point attribute (os.lstat st_file_attributes) is the
    check, because islink()/is_symlink() are False for junctions since 3.13;
    S_ISLNK elsewhere. False when the path does not exist.
    """
    try:
        st = os.lstat(path)
    except OSError:
        return False
    if stat.S_ISLNK(st.st_mode):
        return True
    if os.name == "nt":
        return bool(getattr(st, "st_file_attributes", 0) & REPARSE_POINT)
    return False


def walk_no_links(root):
    """os.walk that prunes symlink/junction dirs and yields no symlink/junction files."""
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames[:] = [d for d in dirnames if not is_link_or_junction(os.path.join(dirpath, d))]
        yield dirpath, dirnames, [f for f in filenames if not is_link_or_junction(os.path.join(dirpath, f))]

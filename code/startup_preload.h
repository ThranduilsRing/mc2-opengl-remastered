// Startup preload: a background IO thread that reads the mission-independent archives and the
// mod-winning data files into the OS file cache while the intro videos and main menu run, so the
// first mission load does not pay cold-disk reads.  Pure IO: it never touches engine registries.
#pragma once

// Call once after the FastFile list is open and the mod VFS is mounted (main thread).
void startStartupPreload();
// Join the worker; safe when it never started.  Call before FastFileFini.
void stopStartupPreload();

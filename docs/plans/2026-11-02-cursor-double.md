# Cursor double (lane10, branch claude/cursor-double)

Symptom (Vulkan): white arrow over the HUD plus the game cursor sprite under the HUD.

Root causes
1. Arrow over HUD: GuiRuntime::Render set io.MouseDrawCursor whenever ImGui WantCaptureMouse was true, so ImGui painted its own white arrow into the foreground list. The OS cursor hide was also only applied at window creation.
2. Sprite under HUD: UserInput::render() drew the sprite into the legacy HUD batch, which flushes BEFORE the ImGui pass; the post-ImGui GL callback never runs under Vulkan.

Fix
- UserInput::captureCursorQuad() captures the sprite quad; mechcmd2_frame registers GuiRuntime::RegisterForegroundCursor, run last in GuiRuntime::Render, drawing via ImGui foreground draw list (topmost 2D layer). The HUD-batch draw is skipped when the ImGui frame is active. ImGui's own arrow is suppressed when the game cursor drew.
- graphics::apply_os_cursor_policy(): one place sets SDL_ShowCursor from focus and MC2_OS_CURSOR; called on focus gained/lost, enter, resize and fullscreen/window-mode changes; logs `[CURSOR] os_cursor event=...`.
- MC2_OS_CURSOR=1 unchanged: OS cursor kept, software cursor skipped. gos_SetHudScaleExempt kept for the legacy (non-captured) path.

# External Player Implementation Plan

> Execute independent units in this session with subagents; review the integrated result before delivery.

**Goal:** Launch installed VLC/PotPlayer and preserve Jellyfin resume/progress through the existing playback reporting path.

**Architecture:** Independent Qt Core/Network process controller, thin application-settings/QWebChannel adapter, and a dedicated native web player plugin. Keep embedded playback the default.

**Tech Stack:** C++/Qt 6, Windows messaging, VLC HTTP JSON, native JavaScript and Node tests.

1. Write lifecycle and launch-validation regression tests for `src/player/ExternalPlayerController.{h,cpp}`. Implement authenticated loopback VLC polling, owned process cleanup and Windows PotPlayer process/window polling. Session IDs guard all asynchronous callbacks.
2. Write Node tests for `native/externalVideoPlayer.js`. Implement the playbackManager plugin contract, preserving final position during `stopped`, suppressing manual-close auto-next, avoiding duplicate reports, and exposing only supported capabilities.
3. Add `src/player/ExternalPlayerComponent.{h,cpp}`, register it in `src/core/ComponentManager.cpp`, and wire it into CMake. Read external player type/path from settings rather than accepting arbitrary launch commands from web content.
4. Add player selection/path settings in `resources/settings/settings_description.json`, plugin loading in `native/nativeshell.js` and script embedding in `src/system/SystemComponent.cpp`. Add test integration and platform usage/troubleshooting documentation.
5. Run `node --test tests/test_externalvideoplayer.js`, the Qt controller tests, actual VLC playback/resume/pause/seek/exit checks, and the fullest build available. Review security and lifecycle issues independently; document exact runtime limits and leave changes committed on `codex/external-player-sync`.

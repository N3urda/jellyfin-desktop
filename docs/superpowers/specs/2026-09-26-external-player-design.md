# External video players with Jellyfin progress

The existing client embeds libmpv. Opening a player URL alone cannot report progress. Add a native adapter that owns an external process and reads its actual playhead; feed those observations into a dedicated jellyfin-web media-player plugin so the existing authenticated session reporting remains authoritative.

## Scope and choices

- Default stays embedded MPV; settings select VLC or PotPlayer for video. Audio keeps the existing selection. VLC supports macOS and Windows (and compatible unsandboxed Linux installs); PotPlayer is Windows-only.
- VLC uses its authenticated HTTP interface bound to IPv4 loopback, a random password and an ephemeral port. PotPlayer uses bounded Windows messages addressed only to a window belonging to the launched process. Start a separate instance so unrelated player windows are untouched.
- Resolve installed players or accept a configured executable path. Launch with argument arrays, never a shell. Do not log stream URLs, tokens, command lines or player stderr.
- Resume from the supplied player start ticks. Poll actual position and paused state; preserve the last valid position when the player exits or resets its playhead on stop. Deliver a final stop exactly once, before clearing the JavaScript session. Old callbacks must not affect subsequent media.
- Native control bridge supports pause, resume, seek, volume and stop. External player UI handles advanced audio/subtitle selection. Pass the selected external subtitle at launch when supported. Do not claim SyncPlay or arbitrary header support.
- Missing executable, startup/control timeout and unsupported platform produce actionable errors. No silent fallback that would start a second player.

## Boundaries and validation

A Qt Core/Network controller owns process and control protocols, independently testable without WebEngine/libmpv. A thin component reads application settings and exposes the controller through QWebChannel. A native JavaScript plugin translates lifecycle events to the normal Jellyfin media-player contract. Its tests cover resume, pause/progress, manual close vs EOF, final-position preservation, duplicate stop, stale callbacks and launch failure. Controller tests cover validation, lifecycle and protocol observations; a real installed VLC smoke test covers actual launch/control/exit. Windows PotPlayer and full logged-in Jellyfin flows require their real runtime and must be reported separately from mocks or protocol tests.

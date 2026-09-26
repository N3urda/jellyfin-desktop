# External video players

Jellyfin Desktop can launch an installed VLC or PotPlayer for video and read its playback position back into the normal Jellyfin playback reporting flow. Keep Jellyfin Desktop open while watching: it owns the external player session and reports progress to your server. Embedded MPV remains the default, and the audio player selection is unchanged.

## Setup

Install the player separately, select it in Jellyfin Desktop's client settings, and restart Jellyfin Desktop. The relevant settings are:

| Setting | Values | Meaning |
| --- | --- | --- |
| `main.externalVideoPlayer` | `disabled`, `vlc`, `potplayer` | `disabled` uses embedded playback; the other values select an installed external video player. |
| `main.vlcPath` | Empty or an executable path | Empty enables VLC discovery; set the path when VLC is installed elsewhere. |
| `main.potPlayerPath` | Empty or an executable path | Empty enables PotPlayer discovery on Windows; set the path for a custom installation. |

Examples of executable paths are `/Applications/VLC.app/Contents/MacOS/VLC`, `C:\Program Files\VideoLAN\VLC\vlc.exe`, and `C:\Program Files\DAUM\PotPlayer\PotPlayerMini64.exe`. Paths are executable locations, not shell commands; do not add command-line arguments to them.

Settings are stored in the active profile's `jellyfin-desktop.conf`, under `sections.main`. See [file locations](../README.md#file-locations). If editing the file manually, quit Jellyfin Desktop first, preserve the existing `version` and other settings, update only the relevant fields, and restart. For example, the fields inside `sections.main` can be:

```json
{
  "externalVideoPlayer": "vlc",
  "vlcPath": "",
  "potPlayerPath": ""
}
```

Choose `disabled` and restart to return to embedded playback. Selecting an unavailable player produces an error; installing a player is a separate step.

## Platform and playback scope

| Player | Windows | macOS | Notes |
| --- | --- | --- | --- |
| VLC | Supported integration target | Supported integration target | Uses the installed desktop VLC and its bundled Lua HTTP interface. |
| PotPlayer | Supported integration target | Unavailable | Uses Windows window messages; current-version Windows runtime validation is required. |

VLC discovery and process control can also work on compatible unsandboxed Linux installations, but that does not establish support for Flatpak, Snap, or other sandboxed player launches.

The integration passes the initial resume position and observes the actual position and pause state. Basic controls include pause, resume, seek, volume, and stop. Volume and mute commands work from Jellyfin, but changes made in the external player's UI are not reflected in Jellyfin's volume display. Use the external player's UI for advanced audio/subtitle selection and rendering settings. An external subtitle selected in Jellyfin is passed at launch when the adapter supports it; this is not a promise of full track switching parity with embedded MPV.

SyncPlay is not supported. Neither arbitrary HTTP request headers nor every server-side transcoding, live TV, codec, subtitle, or authentication configuration is covered. The external player opens the supplied stream URL with its own network and decoder capabilities. Jellyfin Desktop's network settings, including certificate exceptions, do not automatically configure the external player.

Progress is polled every 500 ms, so a process crash can lose the small interval since the last successful observation. Closing the player window preserves the last valid position and suppresses queue auto-advance. An observed stopped state within two seconds of the duration is treated as natural completion; pressing Stop inside the external player during that final interval cannot reliably be distinguished from EOF. Playback completion and episode progression also depend on Jellyfin's normal playback handling. Keep the owned player window on the item Jellyfin launched; opening unrelated files in it is outside the supported session workflow, and PotPlayer currently has no media-identity check.

## Why a native adapter is necessary

Opening a URL with an operating-system file association starts a player but provides no portable progress callback. The adapter therefore starts a separate process and translates observed playback state into Jellyfin's existing media-player events. It does not estimate watched time from wall-clock time.

| Approach | Progress and control | Tradeoff |
| --- | --- | --- |
| Open URL / file association | No standard desktop progress result | Insufficient for reliable resume synchronization. |
| VLC HTTP interface | Structured state plus playback commands | Portable desktop interface; requires an authenticated local endpoint. |
| VLC RC interface | Text commands and state output | Viable alternative, but parsing and platform-specific console/socket behavior add complexity. |
| PotPlayer window messages | Numeric playback queries and commands | Windows only; requires process-owned window discovery and version compatibility checks. |
| Embedded libmpv | Existing in-process playback | Remains the default with the existing integrated feature set. |

VLC documents status, seek, and control requests in its [HTTP request reference](https://github.com/videolan/vlc/blob/3.0.x/share/lua/http/requests/README.txt). Its [HTTP implementation](https://github.com/videolan/vlc/blob/3.0.x/share/lua/intf/modules/httprequests.lua) exposes position, duration, playlist identity, and state; time values become zero after an input disappears, so the adapter retains the last valid observation on exit. VLC's [RC implementation](https://github.com/videolan/vlc/blob/3.0.x/modules/control/oldrc.c) explains the alternate text interface and platform-specific options.

For VLC, the control endpoint binds to `127.0.0.1` with a fresh port and password per session. Native code accesses it; the web client does not receive its credentials. VLC's [HTTP interface source](https://github.com/videolan/vlc/blob/3.0.x/share/lua/intf/http.lua) requires a password. The adapter does not require enabling a persistent web interface in VLC preferences. Windows can disable single-instance forwarding with a launch flag; that flag must be omitted on macOS because VLC's [option registration](https://github.com/videolan/vlc/blob/3.0.x/src/libvlc-module.c) does not provide it there.

PotPlayer's command values are documented by a [copied `InternalSimpleCmd.h` header](https://github.com/ld3l/PotPlayerControl/blob/master/InternalSimpleCmd.h) in a third-party controller project. This is source evidence of the integration contract, not a manufacturer-hosted API stability guarantee. Playback queries use `WM_USER` with commands such as `0x5002` for duration, `0x5004` for current time, and `0x5006` for state. Time values use milliseconds. Older published descriptions disagree on whether stopped is `0` or `-1`, so stopped handling must tolerate both. The `/new` launch option is described in a [transcript of PotPlayer's built-in command-line help](https://github.com/FreeTubeApp/FreeTube/issues/2005); it overrides single-instance preferences.

Only windows belonging to the process launched for this session may receive commands. [GetWindowThreadProcessId](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowthreadprocessid) identifies that ownership, and [SendMessageTimeoutW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw) bounds calls when the player becomes unresponsive. Run PotPlayer and Jellyfin Desktop at ordinary user privilege: Windows [UIPI restrictions](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagew) can block control of an elevated player.

Android has a different mechanism: Jellyfin's [Android external-player bridge](https://github.com/jellyfin/jellyfin-android/blob/master/app/src/main/java/org/jellyfin/mobile/bridge/ExternalPlayer.kt) launches a player activity with resume extras and parses its returned playback position. Desktop VLC and PotPlayer do not implement that Android activity-result contract; the native adapters provide the missing observation channel.

## Security and diagnostics

Player processes receive media URLs, which can include Jellyfin credentials. Do not share full URLs, player command lines, or unredacted external-player logs. An external player may save recent URLs according to its own preferences; local process inspection can also expose launch arguments. The local VLC control password protects that temporary control endpoint, not the Jellyfin media URL.

Player paths are passed directly to process creation with separate argument values. The native adapter does not run a shell, attach to unrelated player windows, or forward player output into application logs. Its errors and runtime web error payloads omit media URLs. This is not a guarantee that upstream jellyfin-web or the external player never logs a URL: upstream startup-failure handling can log its stream descriptor. Inspect and redact logs before sharing them.

If VLC starts but synchronization fails, check that the installation includes the Lua HTTP interface and that localhost connections are allowed. If PotPlayer starts but synchronization fails, check for an elevated process, an unsupported version, or a startup dialog preventing playback. A missing player, startup timeout, or control failure should be surfaced rather than silently opening another player.

## Verification boundaries

Validated on macOS 26.6.2 arm64 with Qt 6.11.2 and VLC 3.0.24:

- The full desktop application builds, and all six CTest targets pass, including the existing application tests, the native controller suite, and the JavaScript adapter suite.
- Native tests use a separate simulated VLC HTTP process to exercise arguments, session isolation, exit positions, startup/control timeouts, and bounded shutdown.
- The actual VLC smoke test passes against a generated 20-second local video: app-bundle launch, resume near three seconds, observed pause/resume, seek near eight seconds, final position on stop, and a second launch through natural completion. Volume/mute commands are sent without errors; the smoke test does not measure the resulting audio output.
- The development application launches and renders the Jellyfin web client with VLC selected in an isolated test configuration. Authenticated media playback and server-side saved progress have **not** been validated.

Windows VLC/PotPlayer integration has **not** been tested with real players on a Windows machine. Release build and installer checks are recorded separately in each release. Older macOS versions, sandboxed distribution, and remote streams/subtitles still need the checks below. The local Homebrew-linked development build is not a portable release artifact.

Repeat the tests from the repository root:

```sh
cmake --build build -j 6
ctest --test-dir build --output-on-failure
node --test tests/test_externalvideoplayer.js
# Use an absolute local video path, longer than ten seconds.
EXTERNAL_PLAYER_VLC_SMOKE=/absolute/path/test.mp4 build/tests/test_externalplayercontroller realVlcSmoke
```

For a controller-only build without WebEngine/libmpv:

```sh
cmake -S tests/external-player -B build/external-controller -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build/external-controller
ctest --test-dir build/external-controller --output-on-failure
```

On this Homebrew setup, Qt application tests require `QT_PLUGIN_PATH=/opt/homebrew/share/qt/plugins`; headless tests also use `QT_QPA_PLATFORM=offscreen`. Running the development app additionally uses `QML_IMPORT_PATH=/opt/homebrew/share/qt/qml`.

For Windows manual verification, use both VLC and PotPlayer where installed:

1. Start a separate player instance with unrelated media, then start a Jellyfin video. Confirm the unrelated instance is unchanged and only the new instance is controlled.
2. Resume a partially watched video. Confirm the external playhead starts near the server's saved position and advances in Jellyfin's active session.
3. Pause and resume from each application and seek forward and backward. Confirm reported playhead/pause state follows the real player without a second playback session. Change volume/mute from Jellyfin and confirm the external player responds; external volume changes need not update Jellyfin's volume display.
4. Close the external player midway. Refresh the item or reopen it from another Jellyfin client and verify the saved resume position. Confirm the next episode does not start because of a manual close.
5. Let a short video finish naturally. Verify final reporting and the expected episode/queue behavior without duplicate stop events.
6. Stop playback in Jellyfin, immediately start another item, and repeat rapidly. Confirm old responses cannot move or stop the new session.
7. Test a missing executable, a playback URL that fails, and an unresponsive or externally terminated player. Confirm an actionable error, no false completed state, and no orphan session.
8. Test a selected external subtitle, non-ASCII paths/titles, and a custom executable path containing spaces. Confirm advanced track controls remain usable in the player.
9. Repeat the resume and final-reporting checks against a real authenticated Jellyfin server. Confirm logs do not contain the stream token or VLC control password.

On macOS, perform the equivalent VLC checks and confirm launching the app bundle's executable works without the unsupported `--no-one-instance` flag. Verify that a separately opened VLC window remains untouched.

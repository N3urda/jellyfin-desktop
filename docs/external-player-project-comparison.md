# External-player integration: Jellyfin Desktop and Fladder

Reviewed 2026-09-26 for the requirement: installed VLC on Windows/macOS and PotPlayer on Windows, with initial resume, observed progress and final stop reporting to Jellyfin.

## Recommendation

For this focused change, Jellyfin Desktop offers the more direct native integration path. This is an architectural judgment, not a measured development-time benchmark. Both projects need new player adapters; neither upstream baseline already satisfies the complete requirement. Fladder remains a reasonable choice if its independent Flutter interface is the preferred long-term product.

| Concern | Jellyfin Desktop, baseline 2cb4a44 | Fladder v0.11.1, a30343a |
| --- | --- | --- |
| Existing playback | Qt WebEngine + embedded libmpv | Flutter with embedded MPV/MDK; Android native backend |
| Installed VLC/PotPlayer tracking | Requires new adapter | Requires new adapter |
| VLC launch and HTTP polling | QProcess and Qt Network in existing C++ layer | Dart process/HTTP APIs can implement this; Flutter itself is no obstacle |
| PotPlayer control | Windows calls fit directly in existing C++ layer | Needs a native platform channel or Windows FFI integration |
| Jellyfin reporting | Reuse jellyfin-web media-player events through QWebChannel | Reuse BasePlayer state stream and the app's own playback models/API calls |
| UI customization | Small settings/panel change fits; larger changes depend on jellyfin-web | Own Flutter UI is convenient for broader client customization |
| macOS distribution | Validate the chosen signed/bundled app's launch behavior | Release entitlements explicitly enable App Sandbox; external launch/process ownership needs a tested design |

Fladder's reporting abstraction is a real advantage: `DirectPlaybackModel` already implements start/progress/stop calls, and `BasePlayer` exposes position/state and player commands. This does not supply observations from VLC or PotPlayer; those protocols still have to be implemented.

## Version-specific evidence

- [Fladder v0.11.1 release](https://github.com/DonutWare/Fladder/releases/tag/v0.11.1) was inspected along with its exact tag, not just the current README.
- [PlayerOptions](https://github.com/DonutWare/Fladder/blob/v0.11.1/lib/models/settings/video_player_settings.dart) lists libMDK, libMPV and nativePlayer; desktop options are MPV and MDK.
- [BasePlayer](https://github.com/DonutWare/Fladder/blob/v0.11.1/lib/wrappers/players/base_player.dart) is an extensible backend interface.
- [DirectPlaybackModel](https://github.com/DonutWare/Fladder/blob/v0.11.1/lib/models/playback/direct_playback_model.dart) owns Jellyfin start/progress/stopped API calls.
- [macOS release entitlements](https://github.com/DonutWare/Fladder/blob/v0.11.1/macos/Runner/Release.entitlements) enable App Sandbox. This is a deployment concern, not evidence that external playback is impossible.
- [Fladder PR #884](https://github.com/DonutWare/Fladder/pull/884) was open and unmerged at review time. Its helper launches Windows Energy Player through a URI containing a stream, subtitle and start time. The reviewed code contains no polling or return channel to send subsequent playback progress back. Its description of position synchronization must not be mistaken for full bidirectional tracking.

Fladder was reviewed from source only; no Fladder binary or player integration was runtime-tested. See [external player setup and verification](external-players.md) for the current Jellyfin Desktop branch's separate evidence and limitations.

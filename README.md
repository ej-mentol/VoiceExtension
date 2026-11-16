# VoiceExtension

Voice data injection plugin and IPC audio pipe for MetaHookSv (Sven Co-op / GoldSrc).

---

## Key Features

- **Direct Buffer Injection**: Injects Opus voice packets directly into the outgoing network buffer in `hw.dll` (`*g_voiceSendData`), without emulating `+voicerecord` keybinds.
- **Dynamic Packet Stamping**: GoldSrc voice envelopes (SteamID64, sequence numbers, CRC32) are stamped at transmission time in `InjectDueChunks` to preserve monotonic sequence numbering.
- **Voice Referee (Mic Coordination)**: Tracks the player's microphone and plugin stream independently. When the physical mic key is pressed, plugin audio is ducked and resumes after release with debounce handling.
- **Priority Audio Queue**: Supports `PASSIVE`, `NORMAL`, `HIGH`, and `URGENT` priorities with preemption for urgent tracks.
- **SteamID Resolution**: Resolves the client's SteamID via `SteamUser020`, pausing injection if SteamID is uninitialized (`0`).
- **Engine State & Sizebuf Verification**: Validates `SIGNONSTATE_FULL` (state 5) and buffer boundaries before writing packets.
- **Auto-Recovery**: Handles map transitions and reconnects without leaving dangling streams or panics.
- **Non-blocking Audio Decoding**: Audio files (WAV/MP3/OGG) and memory PCM are decoded and encoded to Opus outside the engine lock.
- **NamedPipe Server (`\\.\pipe\VoiceExtension`)**: Local IPC interface for external tools (TTS engines, soundboards) supporting concurrent clients and streaming chunks.
- **C++ Interfaces (`IVoiceExtension` / `IVoiceExtension2` / `IVoiceExtension3`)**: Exported via MetaHook's `Sys_GetFactory` (`VoiceExtension_API_001`, `002`, `003`) with client registration and track ownership.

---

## Priority System

| Priority | Level | Behavior |
|---|---|---|
| `PASSIVE` | `0` | Background music, ambient soundscapes. Yields immediately to any other playback. |
| `NORMAL`  | `1` | Standard soundpad sounds, voice lines, dialogue clips. |
| `HIGH`    | `2` | High-priority notifications and chat triggers. Sorted ahead in the waiting queue (does not preempt the currently playing sound). |
| `URGENT`  | `3` | Real-time text-to-speech, critical warnings. Immediately preempts and pauses the currently playing sound, resuming it when the urgent sound completes. |

---

## NamedPipe Protocol (`\\.\pipe\VoiceExtension`)

Operates in message mode (`PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE`).

### 1. Binary Protocol (`VoicePipeHeader`)

Each message begins with the 22-byte packed header (`#pragma pack(push, 1)`):

```cpp
#pragma pack(push, 1)
struct VoicePipeHeader
{
    uint32_t magic;         // 0x56455854 ("VEXT")
    uint16_t command;       // VoicePipeCommand
    uint16_t sampleRate;    // 8000 .. 48000 (e.g. 16000, 24000, 44100, 48000)
    uint8_t  channels;      // 1 = mono, 2 = stereo (automatically downmixed)
    uint8_t  priority;      // 0 = PASSIVE, 1 = NORMAL, 2 = HIGH, 3 = URGENT
    float    volume;        // 0.0 .. 2.0 (clamped)
    float    pitch;         // 0.5 .. 2.0 (clamped)
    uint32_t dataBytes;     // Exact byte size of the payload following this header
};
#pragma pack(pop)
```

#### Binary Commands:
- `1 = PIPE_CMD_PLAY_FILE`: payload is a relative path or absolute path string.
- `2 = PIPE_CMD_PLAY_PCM`: payload is raw 16-bit signed PCM audio.
- `3 = PIPE_CMD_STREAM_CHUNK`: payload is a chunk of continuous live PCM. Only one client owns the live stream at a time.
- `4 = PIPE_CMD_STOP`: stops audio and clears queues owned by this client connection.
- `5 = PIPE_CMD_PAUSE`: pauses current playback.
- `6 = PIPE_CMD_STREAM_END`: flushes and closes the stream owned by this client.
- `7 = PIPE_CMD_RESUME`: resumes paused playback.

### 2. Text Protocol

- `PLAY "<path>" [volume] [pitch] [priority]`
- `STOP`
- `PAUSE [1|0]`
- `RESUME`
- `CLEAR`
- `STREAM_END` / `CLOSE_STREAM`

*Note: For security, paths in `PLAY` are validated against path traversal (`..`). Relative paths resolve against official sound search paths (`svencoop_addon/sound/`, `svencoop_downloads/sound/`, `svencoop/sound/`, `valve/sound/`). Direct absolute paths (e.g. `C:/sounds/clip.wav`) are permitted if the file exists on disk and has an approved audio extension (.wav, .mp3, .ogg).*

---

## In-Game Console Commands & Cvars

### Commands
- `ve_play <sound_path> [volume] [pitch] [priority]`: Enqueue a sound file from console (gated by `ve_allow_server`).
- `ve_serverplay <sound_path> [volume] [pitch] [priority]`: Server-triggered playback alias (gated by `ve_allow_server`).
- `ve_stop`: Stop all active and paused sounds and clear queue. Always available as an emergency player kill-switch (like engine `stopsound`).
- `ve_pause`: Pause playback (gated by `ve_allow_server`).
- `ve_resume`: Resume paused playback (gated by `ve_allow_server`).
- `ve_status`: Display comprehensive telemetry report (engine hooks, client state, buffer status, sent chunks, drop counters, and jitter metrics). Always available.
- `ve_panic [reason]`: Force manual panic mode (disables injection and clears queues; gated by `ve_allow_server`).
- `ve_unpanic`: Clear panic mode and restore playback readiness. Always available.

### Cvars
- `ve_duck_resume_ms` (default: `150`): Delay in milliseconds before resuming sound after physical microphone key is released, allowing key-switch bounce to settle cleanly.
- `ve_allow_abs_paths` (default: `0`): Sandbox security control. When `0`, all absolute drive-letter paths (e.g. `C:/...`) are rejected. When `1`, absolute paths are permitted solely when received via the local Named Pipe interface (e.g. for external TTS daemons writing temporary audio files). All console and plugin API inputs remain strictly restricted to the game sandbox.
- `ve_allow_server` (default: `0`): Security gate for the engine command buffer. When `0`, all action commands executed via the console or injected by rogue game servers via `stuffcmd` are ignored. Set to `1` only if you explicitly want to run playback or commands via console/keybinds. **Note**: C++ plugins (such as Soundpad) and external tools connected via Named Pipe bypass this restriction entirely and do not require `ve_allow_server`.

---

## Building

- **Platform & Architecture**: Windows 32-bit (`x86` / `Win32`). 64-bit builds are unsupported as the GoldSrc / Sven Co-op engine and MetaHookSv run as 32-bit processes.
- **Toolchain**: Microsoft Visual Studio 2022 (MSVC toolset `v143` or `v145`), ISO C++17 standard.
- **Dependencies**:
  - MetaHookSv SDK headers (`metahook.h`, `Interface/IFileSystem.h`).
  - **libopus 1.6.1**: compiled as an x86 static library (`Win32` / MSVC). Must be configured with static CRT matching VoiceExtension (`CMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>"` for `/MT` Release and `/MTd` Debug) to prevent CRT linkage collisions. Standard options (default x86 intrinsics, DRED/QEXT disabled). Place the compiled library at `lib/opus.lib`. The headers in `include/opus/` match this 1.6.1 release.
  - Encoder settings configured in `VoiceCore`: `OPUS_APPLICATION_VOIP`, 32 kbps bitrate, complexity 5, `OPUS_SIGNAL_VOICE`, 24 kHz mono (20ms Opus frames, 1..8 frames per GoldSrc VoIP format-11 block, matching the engine's own ~50ms cadence).
- **Build Methods**:
  - Open `VoiceExtension.vcxproj` in Visual Studio and select **Release | Win32**.
  - Or run `build_plugin.bat` / `build_plugin.ps1` from a developer command prompt.

---

## Known Limitations & Operational Notes

- **Engine Compatibility**: `Voice_Send` is located by signature scan and its `sizebuf` pointers are validated at runtime (capacity and bounds sanity checks). If the layout does not match, injection is refused and the reason is reported instead of writing to unexpected memory.
- **Steam Authentication**: Voice packet injection relies on resolving the local client's SteamID via `SteamUser020`. If Steam is uninitialized or the client is not connected to a server (SteamID is 0), injection is paused to prevent server drop.
- **Sandbox Security**: Sound files must reside under valid game sound paths (`svencoop_addon/sound/`, `svencoop_downloads/sound/`, `svencoop/sound/`, `valve/sound/`). Absolute drive-letter paths are rejected unless received via local Named Pipe with `ve_allow_abs_paths 1`.
- **Microphone Ducking**: When the physical microphone key is pressed (`+voicerecord`), plugin transmission transitions to `DUCKED` (playback is paused and live microphone passes through). Playback resumes automatically `ve_duck_resume_ms` after releasing the key.
- **State Machine**: States `PREROLL` and `POSTROLL` are reserved in the ABI and are not emitted during direct injection.

---

## Documentation & Licenses

- [Architecture Overview](docs/architecture.md)
- [NamedPipe Protocol Details](docs/pipe_protocol.md)
- [C++ API Reference](docs/api_reference.md)
- [License (MIT)](LICENSE)
- [Third-Party Notices](THIRD_PARTY_NOTICES.md)

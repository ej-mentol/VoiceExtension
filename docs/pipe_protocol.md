# NamedPipe IPC Protocol Reference

Pipe Name: `\\.\pipe\VoiceExtension`

The pipe server operates in `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE` mode, supporting up to 8 concurrent client connections. Overlapped I/O with automatic packet reassembly handles arbitrary payload sizes up to 16 MB.

---

## 1. Binary Protocol

Every binary message begins with a 22-byte packed header (`#pragma pack(push, 1)`):

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

### Commands

| Value | Name | Description |
|---|---|---|
| `1` | `PIPE_CMD_PLAY_FILE` | Payload contains UTF-8 path string. Resolved relative to game sound directories, or absolute path when `ve_allow_abs_paths 1` is enabled. |
| `2` | `PIPE_CMD_PLAY_PCM` | Payload contains raw 16-bit signed PCM audio. Resampled and queued under the connection's client ID. |
| `3` | `PIPE_CMD_STREAM_CHUNK` | Pushes a PCM audio segment into a live continuous stream. Opens the stream if not already active. Only one connection may own the live stream at a time. |
| `4` | `PIPE_CMD_STOP` | Stops playback and clears queues owned by this client connection, without interrupting other plugins. |
| `5` | `PIPE_CMD_PAUSE` | Pauses audio playback without advancing timestamps. |
| `6` | `PIPE_CMD_STREAM_END` | Flushes trailing samples, pads final packet, and closes the active stream owned by this connection. |
| `7` | `PIPE_CMD_RESUME` | Resumes paused audio. |

---

## 2. Text Protocol

Clients can also send newline-terminated UTF-8 ASCII text lines:

- `PLAY "<path>" [volume] [pitch] [priority]`
  - Enqueues an audio file. Quotes are supported for paths containing spaces.
- `STOP`
  - Stops active playback and clears queued tracks owned by this client connection.
- `PAUSE [1|0]`
  - Pauses (`1`) or unpauses (`0`) current playback.
- `RESUME`
  - Resumes paused playback.
- `CLEAR`
  - Clears queued tracks owned by this client connection.
- `STREAM_END` / `CLOSE_STREAM`
  - Flushes and closes the active stream owned by this connection.

---

## 3. Disconnection Handling

When a pipe client connection closes or disconnects:
- If the client owned an active live stream, the stream is cleanly closed (`UnregisterClient(clientId, false)`) so that live sessions do not hang.
- Already enqueued non-stream audio tracks (`PLAY_FILE`, `PLAY_PCM`) continue playing to completion, ensuring one-shot scripts and external commands are not abruptly cut off.
- The client ID is detached from the active client registry, and internal safeguards prevent the client ID from being reassigned to new connections until all its playing and queued tracks have finished.
- If a streaming producer stalls without closing the stream, an internal 2000ms idle watchdog automatically closes the stream and advances the playback queue.

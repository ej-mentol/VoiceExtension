# VoiceExtension Architecture

## Overview

`VoiceExtension` is a MetaHookSv plugin that injects voice packets and audio playback directly into the GoldSrc client network stream (Sven Co-op).

Instead of emulating `+voicerecord` console commands, it writes Opus voice packets directly into the engine's outgoing `sizebuf_t` network buffers within `hw.dll`.

```
               [External Client / TTS / Bot]
                             │
                             ▼
         NamedPipe (\\.\pipe\VoiceExtension)
                             │
                  [VoicePipeServer (I/O)]
                             │
                             ▼
                    [VoiceCore Engine]
                    ├── Path Sanitizer & Decoders (WAV/MP3/OGG)
                    ├── Linear Resampler & 2nd-order Butterworth LPF
                    ├── Dedicated Local Opus VoIP Encoders (24kHz)
                    └── Priority Track Queue (PASSIVE..URGENT)
                             │
                             ▼
              [Hook: hw.dll!Voice_Send]
                             │
            Inject stamped GoldSrc voice frames
            (SteamID64, format 11, seq, 20ms Opus, CRC32; 1..8 frames per block)
                             │
                             ▼
                 [GoldSrc Network Stream]
```

## Core Components

### 1. Direct Buffer Injection (`Voice_Send`)
- Hooks `hw.dll`'s `Voice_Send` function.
- Rather than simulating keypresses, it places pre-encoded Opus payloads with full GoldSrc envelope (`clc_voicedata` opcode `8`, SteamID64, sample rate `24000`, monotonic sequence numbers, and CRC32) directly into the outgoing `sizebuf_t` buffer.
- Dynamic pointer scanning inside `Voice_Send` resolves the engine `maxsize`, `cursize`, and data pointers across builds, falling back to the RVAs recorded in the source and validated at runtime, with explicit diagnostic error reporting.

### 2. Lock-Free Audio Encoding
- Audio decoding (WAV, MP3 via minimp3, OGG via stb_vorbis) and Opus encoding for disk files and memory PCM execute **completely outside `m_lock`**.
- Each encode operation uses an independent `OpusEncoderPtr` instance.
- This prevents game thread micro-stutters when enqueuing large audio tracks.
- Live streaming sessions maintain their own dedicated Opus encoder protected by `m_lock` and an idle watchdog.

### 2a. Voice Block Pacing
- Audio is stored as 20ms Opus frames (`kFrameDurationUs = 20000`).
- `Voice_Send` is called by the engine at its command cadence (~50ms in the observed
  Sven Co-op client); Steam `GetVoice` therefore returns 1..8 frames per call
  (median interval ~50ms, average 2.5 frames per block in observed engine traffic).
- `InjectDueChunks` mirrors that shape: once per call it packs every due frame (up to 8)
  into **one** `clc_voicedata` block, with an 80ms lookahead so a 50ms tick always has
  2..3 frames available. It never emits two separate blocks in one tick for steady-state
  playback.
- `ve_status` reports `injectedFrames` and `injectedPackets` separately so the block shape
  can be checked in game.

### 3. Voice Referee (Microphone Coordination)
- Hooks `ISteamUser020` methods (`StartVoiceRecording`, `StopVoiceRecording`, `GetVoice`, `GetAvailableVoice`, `DecompressVoice`).
- When the player presses their physical push-to-talk key:
  - Playing audio streams with `allowDucking = true` are immediately transitioned to `DUCKED`.
  - Transmission flags are dropped so physical microphone audio flows directly to the server without collision.
  - When the user releases the microphone key, an adjustable debounce delay (`ve_duck_resume_ms`, default 150ms) suppresses key-bounce jitter before cadence-correct resumption.

### 4. Watchdogs & Auto-Recovery Hysteresis
- **State Transition & Map Load (`clientState != 5`):**
  - When the client state is not in-game (5), `VoiceCore` immediately executes `StopAll()` without locking the engine in an error state. This safely flushes pending queues, closes transmission, and resets the cached SteamID so audio tails do not spill into loading screens or across servers.
- **Recoverable Panic:**
  - If SteamID remains `0` for more than 1000ms during active playback, or if the engine sizebuf stalls unflushed for >250ms (transient rate choke / frame lag):
  - Active queues are cleared and listeners notified of track termination.
  - The core enters a recoverable panic state.
  - `TryAutoRecover()` continuously monitors engine health (connected state, valid sizebuf capacity, and non-zero SteamID64).
  - Once the engine has remained continuously healthy for 1000ms, the panic is automatically cleared with zero user intervention.
- **Latched Panic:**
  - Irrecoverable layout violations (sizebuf cursor overflow or corrupted engine structure pointers) enter a fatal latched panic that requires manual verification (`ve_unpanic`).

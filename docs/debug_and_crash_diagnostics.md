# Debug & Crash Diagnostics Guide (VoiceExtension)

This document covers live logging through Microsoft Sysinternals DebugView
(`Dbgview.exe`) and the standalone test checks under `tests/`.

---

## 1. Quick Start: DebugView

Logging is disabled by default. Set `ve_debug 1` in the console to enable it;
`ve_debug 0` turns it off again. Every stage from plugin start to path
normalization, decoding and packet injection then emits a `DebuLog` line.

`DebuLog` writes through `OutputDebugStringA`, so DebugView shows lines as they
are produced. A hard crash can still cut the last few lines before they are
drained by the debugger, so treat the log tail as a hint, not as a guaranteed
trace of the exact faulting instruction.

### Running DebugView
1. Download **DebugView** from Microsoft Sysinternals (or run `Dbgview.exe`).
2. Right-click and choose **Run as Administrator** (required for global Win32 capture).
3. In the **Capture** menu:
   - Check **Capture Win32**
   - Check **Capture Global Win32**
   - Check **Pass-Through**
   - (Optional) Check **Capture Kernel** only when debugging driver-level conflicts
4. In **Edit -> Filter/Highlight** (`Ctrl+L`) set **Include** to `[VE]*`
   (or leave `*` to see everything) and click **OK**.

### Log Format
Each line carries a subsystem tag and the OS thread id (`TID`):

```text
[VE][TID:4288] CVoiceExtensionPlugin::Init: api=<addr>, pInterface=<addr>
[VE][TID:4288] InstallVoiceSendHook: SUCCESS (maxsize=65487)
[VE][TID:4288] IsPathSafe: ACCEPTED 'sound/ambient/test.wav'
[VE][TID:4288] ResolveSoundPath: IFileSystem resolved to local path 'svencoop/sound/ambient/test.wav'
[VE][TID:4288] AudioDecoder::DecodeFile: 'svencoop/sound/ambient/test.wav'
[VE][TID:4288] AudioDecoder::DecodeMemory: identified as WAV container
[VE][TID:4288] EnqueueFile: resolved='svencoop/sound/ambient/test.wav', chunks=12, prio=1
```

---

## 2. `DebuLog` Reference

Defined in `src/DebugLog.h`.

- **One-line call**: `DebuLog("Starting file decode: %s", path);`
- **Gated**: does nothing unless `ve_debug` is greater than zero.
- **Thread id**: prefixes `[VE][TID:xxxx]`.
- **Newline**: appended when missing.
- **Bounds**: formats into a fixed stack buffer, no heap allocation, no lock.

---

## 3. Tests

`tests/` is a standalone, dependency-free harness (`test_harness.h`) driven by
`test_runner.cpp`. It is not part of the CMake or MSBuild build.

The checks fall into two groups, and the difference matters:

1. **Production code is exercised directly** — `VoiceUtils::IsPathSafe` (path
   traversal, extension whitelist, reserved engine names, DOS device stems,
   absolute-path policy, NTFS alternate streams) and `VoiceUtils::SoftLimit`
   (linear range and saturation curve).
2. **Protocol-layout checks** — the packed `VoicePipeHeader` size/offsets and a
   standard RIFF/WAVE header layout.
3. **Mock-driven invariants** — queue quotas, client-id allocation, stream
   ownership, ducking transitions and DSP cutoff arithmetic are implemented as
   local mocks in the test file. They document intended rules; they cannot catch
   a divergence in `VoiceCore.cpp`.

### Running
```cmd
cl /std:c++17 /EHsc /W4 /I tests tests\test_runner.cpp /Fe:tests\test_runner.exe
tests\test_runner.exe
```

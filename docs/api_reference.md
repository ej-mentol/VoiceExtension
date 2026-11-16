# C++ Interface Reference (`IVoiceExtension` / `IVoiceExtension2` / `IVoiceExtension3`)

Headers:
- `#include "Interface/IVoiceExtension.h"`
- `#include "VoiceExtensionImport.h"`

Interfaces are exposed via MetaHook's `Sys_GetFactory`:
- `"VoiceExtension_API_001"` -> `IVoiceExtension*`
- `"VoiceExtension_API_002"` -> `IVoiceExtension2*`
- `"VoiceExtension_API_003"` -> `IVoiceExtension3*`

---

## Priorities

```cpp
enum class VoicePriority : int32_t
{
    PASSIVE = 0,   // Low priority: background music, ambient loops (yields to everything)
    NORMAL  = 1,   // Standard priority: soundpad sounds, voice lines
    HIGH    = 2,   // High priority: chat triggers, notifications (sorted ahead in queue)
    URGENT  = 3    // Max priority: player text-to-speech, warnings (preempts and pauses current track)
};
```

---

## Methods

### `IVoiceExtension`
```cpp
// 1. One-Shot Operations
uint32_t EnqueueFile(const char* relativePath, const VoicePlayParams& params = {});
uint32_t EnqueuePcm(const int16_t* pcm, size_t samples, int sampleRate, const VoicePlayParams& params = {});

// 2. Playback Control
void StopAll();
void StopCurrent();
void ClearQueue();
void Pause(bool pause);

// 3. Streaming Mode
bool OpenStream(int sampleRate, const VoicePlayParams& params = {});
bool PushStreamPcm(const int16_t* pcm, size_t samples);
void CloseStream();

// 4. Status & Diagnostics
VoicePlaybackState GetState() const;
bool IsTransmitting() const;
size_t GetQueueSize() const;
uint64_t GetCurrentSteamID() const;
bool IsPanic() const;
const char* GetPanicReason() const;

// 5. Event Listeners
void RegisterListener(IVoiceEventListener* listener);
void UnregisterListener(IVoiceEventListener* listener);
```

### `IVoiceExtension2`
Inherits all methods from `IVoiceExtension`, plus:
```cpp
// Returns true when engine hooks are installed, connected to server, and not in panic.
virtual bool IsReady() const = 0;
```

### `IVoiceExtension3`
Inherits all methods from `IVoiceExtension2`, adding multi-client isolation, ownership tagging, and fine-grained track management:
```cpp
// Register a named client (e.g. "Soundpad", "PiperTTS"). Returns a unique clientId (1..254).
virtual uint32_t RegisterClient(const char* clientName) = 0;

// Unregister client and stop all tracks/streams owned by it.
virtual void UnregisterClient(uint32_t clientId) = 0;

// Enqueue operations tagged with owner clientId
virtual uint32_t EnqueueFileEx(uint32_t clientId, const char* relativePath, const VoicePlayParams& params = {}) = 0;
virtual uint32_t EnqueuePcmEx(uint32_t clientId, const int16_t* pcm, size_t samples, int sampleRate, const VoicePlayParams& params = {}) = 0;

// Stream operations tagged with owner clientId
virtual bool OpenStreamEx(uint32_t clientId, int sampleRate, const VoicePlayParams& params = {}) = 0;
virtual bool PushStreamPcmEx(uint32_t clientId, const int16_t* pcm, size_t samples) = 0;
virtual void CloseStreamEx(uint32_t clientId) = 0;
virtual bool IsStreamingActiveEx(uint32_t clientId) const = 0;
virtual uint32_t GetStreamOwner() const = 0;

// Fine-grained cancellation & isolation
virtual bool StopTrack(uint32_t trackId) = 0;
virtual void StopClient(uint32_t clientId) = 0;
virtual size_t GetClientQueueSize(uint32_t clientId) const = 0;
virtual uint32_t GetTrackOwner(uint32_t trackId) const = 0;
```

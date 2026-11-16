#pragma once

#ifndef IVOICE_EXTENSION_H
#define IVOICE_EXTENSION_H

#include <interface.h>
#include <stdint.h>

#define VOICE_EXTENSION_INTERFACE_VERSION "VoiceExtension_API_001"
#define VOICE_EXTENSION_PIPE_NAME "\\\\.\\pipe\\VoiceExtension"

enum class VoicePriority : int32_t
{
	PASSIVE = 0,   // Low priority: background music, ambient loops (yields to everything)
	NORMAL  = 1,   // Standard priority: soundpad sounds, voice lines
	HIGH    = 2,   // High priority: chat triggers, notifications, radio commands
	URGENT  = 3    // Max priority: player text-to-speech, urgent warnings (preempts or ducks others)
};

enum class VoicePlaybackState : int32_t
{
	IDLE = 0,
	PREROLL,       // Reserved (not used; enum value preserved for ABI compatibility)
	PLAYING,       // Active audio packet transmission into hw.dll Voice_Send
	DUCKED,        // User mic key held; plugin transmission is paused and the live mic passes through
	POSTROLL       // Reserved (not used; enum value preserved for ABI compatibility)
};

struct VoicePlayParams
{
	float volume = 1.0f;       // Clean relative volume multiplier (0.0 .. 1.0+)
	float pitch = 1.0f;        // Playback pitch / speed (0.5 .. 2.0)
	VoicePriority priority = VoicePriority::NORMAL;
	bool allowDucking = true;  // Whether physical mic pauses this stream to let live mic audio pass through
	bool loop = false;         // Loop playback until stopped
	uint32_t trackId = 0;      // Optional caller track ID for callback matching
};

// Per-client registration parameters.
// hangtimeMs: how long (milliseconds) VE holds the microphone slot open after the
// client's last queued track finishes, waiting for another track before going IDLE.
// 0 = release immediately (default). Recommended: 200-400 for soundboard clients.
struct VoiceClientParams
{
	uint32_t hangtimeMs = 0;
};

// -------------------------------------------------------------------------
// Event Listener Contract
// -------------------------------------------------------------------------
// 1. Identity Guarantee: If caller specified VoicePlayParams::trackId != 0,
//    all callbacks (OnTrackStarted, OnTrackFinished) echo that exact trackId.
//    If trackId == 0, the core assigned trackId (returned by Enqueue*) is used.
// 2. Playback Activation: OnTrackStarted fires when a track is promoted to active
//    playback. Audio scheduling into hw.dll Voice_Send begins with a slight lead time
//    (~80ms) to populate network/receiver jitter buffers. If the player's push-to-talk
//    key is pressed, playback is ducked immediately until mic release. Note that
//    due to network transport, Opus frame pacing, and client jitter buffers,
//    remote speaker playback is NOT sample-accurate.
// 3. Lifecycle Completion: Any track accepted by Enqueue* (returning non-zero)
//    is GUARANTEED to receive an OnTrackFinished callback when playback completes,
//    when stopped (via StopTrack, StopClient, StopAll, or ClearQueue), superseded,
//    or on plugin shutdown. Clients can safely release per-track resources on OnTrackFinished.
// 4. Thread Safety & Non-Blocking Execution: Listener callbacks can be invoked from
//    multiple thread contexts (engine/game thread, pipe thread, or caller thread).
//    Implementations MUST be thread-safe, return promptly without blocking, offload heavy
//    operations (disk I/O, audio decoding) to background workers, and MUST NOT call
//    back into IVoiceExtension synchronously from within listener methods to avoid deadlocks.
class IVoiceEventListener
{
public:
	virtual void OnTrackStarted(uint32_t trackId, const char* name) {}
	virtual void OnTrackFinished(uint32_t trackId, const char* name) {}
	virtual void OnStateChanged(VoicePlaybackState newState) {}
};

class IVoiceExtension : public IBaseInterface
{
public:
	// -------------------------------------------------------------------------
	// 1. One-Shot Queue Operations
	// -------------------------------------------------------------------------
	// Synchronous Validation Contract:
	// - Performs immediate validation (path safety, file existence/access, format,
	//   per-client queue quotas, engine panic check).
	// - Returns 0 immediately if validation fails, allowing callers to discard state immediately.
	// - Returns unique non-zero core trackId if accepted into playback pipeline.
	// Resolves loose files in: svencoop/sound/, svencoop_downloads/sound/, svencoop_addon/sound/, valve/sound/
	virtual uint32_t EnqueueFile(const char* relativePath, const VoicePlayParams& params = {}) = 0;
	
	// Enqueues pre-rendered or raw PCM (mono, 16-bit, any sample rate - core resamples to 24kHz Opus)
	virtual uint32_t EnqueuePcm(const int16_t* pcm, size_t samples, int sampleRate, const VoicePlayParams& params = {}) = 0;

	// -------------------------------------------------------------------------
	// 2. Playback & Queue Control
	// -------------------------------------------------------------------------
	virtual void StopAll() = 0;                  // Stops everything, clears active and queued tracks
	virtual void StopCurrent() = 0;              // Skips current track, advances queue
	virtual void ClearQueue() = 0;                // Clears upcoming tracks, leaves current playing
	virtual void Pause(bool pause) = 0;

	// -------------------------------------------------------------------------
	// 3. Streaming Mode (Continuous stream from external TTS / AI engines)
	// -------------------------------------------------------------------------
	virtual bool OpenStream(int sampleRate, const VoicePlayParams& params = {}) = 0;
	virtual bool PushStreamPcm(const int16_t* pcm, size_t samples) = 0;
	virtual void CloseStream() = 0;

	// -------------------------------------------------------------------------
	// 4. Status & Diagnostics
	// -------------------------------------------------------------------------
	virtual VoicePlaybackState GetState() const = 0;
	virtual bool IsTransmitting() const = 0;
	virtual size_t GetQueueSize() const = 0;
	virtual uint64_t GetCurrentSteamID() const = 0;

	// -------------------------------------------------------------------------
	// 5. Event Listeners
	// -------------------------------------------------------------------------
	virtual void RegisterListener(IVoiceEventListener* listener) = 0;
	virtual void UnregisterListener(IVoiceEventListener* listener) = 0;
};

#define VOICE_EXTENSION_INTERFACE_VERSION_2 "VoiceExtension_API_002"

// Extended interface adding panic and readiness reporting.
// New virtuals are appended strictly after the API_001 vtable for binary prefix-compatibility.
class IVoiceExtension2 : public IVoiceExtension
{
public:
	// True if the engine is in panic mode (injection disabled)
	virtual bool IsPanic() const = 0;
	// Returns panic description string if IsPanic() is true, else empty string ""
	virtual const char* GetPanicReason() const = 0;
	// True only when engine hooks are active, connected to server (state 5), and not panicked
	virtual bool IsReady() const = 0;
};

#define VOICE_EXTENSION_INTERFACE_VERSION_3 "VoiceExtension_API_003"

// Multi-client interface: provides client isolation, track ownership,
// fine-grained cancellation, and stream arbitration.
// Appended strictly after IVoiceExtension2 for binary prefix-compatibility.
class IVoiceExtension3 : public IVoiceExtension2
{
public:
	// Register a named client (e.g. "Soundpad", "PiperTTS", "DeathNotifier").
	// Returns a unique non-zero clientId.
	virtual uint32_t RegisterClient(const char* clientName, const VoiceClientParams& params = VoiceClientParams()) = 0;

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

	// Fine-grained cancellation. Matches strictly on the core trackId returned by Enqueue*Ex.
	// (callerTrackId in params is echoed exclusively in listener events to avoid cross-client ID collisions).
	// When a track is stopped, OnTrackFinished is dispatched to complete its lifecycle.
	virtual bool StopTrack(uint32_t trackId) = 0;
	virtual void StopClient(uint32_t clientId) = 0;

	// Per-client queue depth query
	virtual size_t GetClientQueueSize(uint32_t clientId) const = 0;

	// Query owner clientId of an active or queued track by its core trackId
	virtual uint32_t GetTrackOwner(uint32_t trackId) const = 0;
};

#endif // IVOICE_EXTENSION_H

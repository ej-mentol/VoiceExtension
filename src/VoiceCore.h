#pragma once

#ifndef VOICE_CORE_H
#define VOICE_CORE_H

#include <metahook.h>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>
#include "Interface/IVoiceExtension.h"

struct OpusEncoder;

// One Opus frame (20ms at 24kHz). Frames are packed into a single engine voice
// block at transmission time, exactly like Steam GetVoice does: the engine emits
// one variable-length block (1..8 frames) per Voice_Send call (~20 Hz).
struct OpusChunk
{
	std::vector<uint8_t> frame;
	uint64_t dueUs = 0;
};

struct VoiceTrack
{
	uint32_t trackId = 0;
	uint32_t ownerId = 0;
	uint32_t callerTrackId = 0;
	std::string name;
	VoicePlayParams params;
	std::vector<OpusChunk> chunks;
	size_t currentChunk = 0;
	bool isStream = false;
	uint64_t submissionOrder = 0;  // Assigned before encoding; used to preserve arrival order within same priority

	uint32_t GetEventTrackId() const { return callerTrackId != 0 ? callerTrackId : trackId; }
};

struct StreamingSession
{
	bool active = false;
	uint32_t ownerId = 0;
	int sourceSampleRate = 24000;
	VoicePlayParams params;
	std::vector<int16_t> pcmAccumulator; // Accumulates incoming samples until a full 20ms frame (480 samples)
	uint64_t nextDueUs = 0;
	double resamplePhase = 0.0;          // Fractional phase continuity between chunk boundaries
};

// Diagnostic telemetry counters.
// NOTE: These counters are diagnostic approximations collected across the engine hook thread,
// pipe worker, and decoding routines without synchronization overhead. They are designed
// for developer observability, logging, and ve_status reporting, not transactional state.
struct VoiceTelemetry
{
	uint64_t injectedFrames = 0;   // 20ms Opus frames written into the engine buffer
	uint64_t injectedPackets = 0;  // CLC_VOICEDATA blocks carrying those frames
	uint64_t droppedStateNot5 = 0;
	uint64_t droppedSteamIdZero = 0;
	uint64_t droppedBufferFull = 0;
	uint64_t unflushedFrames = 0;
	uint64_t rxPackets = 0;
	uint64_t rxBytes = 0;
	uint32_t rxJitterCount = 0;
	uint32_t maxRxGapUs = 0;
};

class VoiceCore
{
public:
	static VoiceCore& Get();

	bool Init();
	void Shutdown();

	// Multi-client management (IVoiceExtension3)
	uint32_t RegisterClient(const char* clientName, const VoiceClientParams& params = VoiceClientParams());
	void UnregisterClient(uint32_t clientId, bool stopTracks = true);
	void ClearClientQueue(uint32_t clientId);

	// Enqueue operations
	uint32_t EnqueueFile(const char* relativePath, const VoicePlayParams& params = {}, bool isFromPipe = false, uint32_t clientId = 0);
	uint32_t EnqueuePcm(const int16_t* pcm, size_t samples, int sampleRate, const VoicePlayParams& params = {}, uint32_t clientId = 0);
	
	// Playback & Queue Control
	void StopAll();
	void StopCurrent();
	void ClearQueue();
	void Pause(bool pause);

	// Multi-client stream arbitration
	bool OpenStream(int sampleRate, const VoicePlayParams& params = {}, uint32_t clientId = 0);
	bool PushStreamPcm(const int16_t* pcm, size_t samples, uint32_t clientId = 0);
	void CloseStream(uint32_t clientId = 0);
	bool IsStreamingActive(uint32_t clientId = 0) const;
	uint32_t GetStreamOwner() const;
	bool IsCurrentTrackOwner(uint32_t clientId) const;

	// Fine-grained cancellation
	bool StopTrack(uint32_t trackId);
	void StopClient(uint32_t clientId);
	size_t GetClientQueueSize(uint32_t clientId) const;
	uint32_t GetTrackOwner(uint32_t trackId) const;

	VoicePlaybackState GetState() const;
	bool IsTransmitting() const;
	size_t GetQueueSize() const;
	uint64_t GetCurrentSteamID() const;

	void RegisterListener(IVoiceEventListener* listener);
	void UnregisterListener(IVoiceEventListener* listener);

	// Engine frame injection (called from hooked Voice_Send)
	bool InjectDueChunks();

	// Voice Referee & microphone tracking
	void SetUserTalking(bool talking);
	bool IsUserTalking() const { return m_bUserTalking.load(std::memory_order_relaxed); }
	bool IsPluginTransmitting() const { return m_bPluginTransmitting.load(std::memory_order_relaxed); }
	void OnVoicePacketObserved(const uint8_t* data, size_t size);

	// Panic mode: disables all injection instantly and clears queues.
	// Transient (recoverable) panic auto-recovers when client reconnects and SteamID is valid.
	// Fatal/latched panic requires explicit Unpanic().
	void Panic(const char* reason, bool recoverable = false);
	void PanicFatal(const char* reason) { Panic(reason, false); }
	void Unpanic();
	bool IsPanic() const { return m_bPanic.load(std::memory_order_relaxed); }
	// Thread-safe copy of panic reason buffer into caller-provided storage.
	void GetPanicReason(char* outBuf, size_t maxLen) const
	{
		if (!outBuf || maxLen == 0)
			return;
		EnterCriticalSection(&m_lock);
		strncpy_s(outBuf, maxLen, m_panicReason, _TRUNCATE);
		LeaveCriticalSection(&m_lock);
	}
	const char* PanicReason() const
	{
		static thread_local char s_reason[256];
		GetPanicReason(s_reason, sizeof(s_reason));
		return s_reason;
	}
	const char* GetPanicReason() const
	{
		return PanicReason();
	}

	// Telemetry & diagnostics
	const VoiceTelemetry& GetTelemetry() const { return m_stats; }
	void OnDecompressVoice(uint32_t writtenBytes);

	// Security & sound path resolution
	static bool IsPathSafe(const char* path, bool isFromPipe = false);
	static std::string ResolveSoundPath(const char* relativePath, bool isFromPipe = false);
	static float SoftLimit(float x);

private:
	VoiceCore();
	~VoiceCore();

	bool EncodeFileToChunks(const char* fullPath, const VoicePlayParams& params, std::vector<OpusChunk>& outChunks);
	bool EncodePcmToChunks(const int16_t* pcm, size_t samples, int sampleRate, const VoicePlayParams& params, std::vector<OpusChunk>& outChunks);
	
	uint32_t InsertTrackLocked(std::unique_ptr<VoiceTrack> track, bool& startedNow, std::string& startedName,
		bool& preemptedStream, uint32_t& preemptedStreamId, std::string& preemptedStreamName,
		bool& droppedPaused, uint32_t& droppedPausedId, std::string& droppedPausedName,
		bool& notifyState, VoicePlaybackState& stateToNotify);
	void AdvanceTrackLocked(bool& notifyNextStarted, uint32_t& nextStartedId, std::string& nextStartedName,
		bool& notifyState, VoicePlaybackState& stateToNotify);
	void DrainStreamAccumulatorLocked(bool flushTail);
	bool TryAutoRecover();
	bool HasClientTracksLocked(uint32_t clientId) const;

	// Packs one engine voice block (1..8 Opus frames) and appends it to the
	// outgoing sizebuf. Sequence numbers are stamped here, at injection time.
	bool StampAndInjectChunk(const OpusChunk* frames, size_t frameCount, uint32_t& sentBytes, uint64_t steamId);

	void NotifyTrackStarted(uint32_t trackId, const char* name);
	void NotifyTrackFinished(uint32_t trackId, const char* name);
	void NotifyStateChanged(VoicePlaybackState newState);

	bool SetStateLocked(VoicePlaybackState newState, VoicePlaybackState& outNotifiedState);
	uint64_t QuerySteamID() const;

private:
	mutable CRITICAL_SECTION m_lock;
	bool m_bInitialized = false;

	OpusEncoder* m_pOpusEncoder = nullptr;
	std::atomic<uint16_t> m_globalSequence{ 1000 };
	uint32_t m_nextClientId = 1;
	std::atomic<uint32_t> m_nextTrackSeq{ 1 };
	std::atomic<uint64_t> m_submissionCounter{ 0 };
	std::unordered_map<uint32_t, std::string> m_registeredClients;
	std::unordered_map<uint32_t, VoiceClientParams> m_clientParams;

	// Playback State
	VoicePlaybackState m_state = VoicePlaybackState::IDLE;
	std::atomic<bool> m_bPaused{ false };
	LARGE_INTEGER m_pauseStartTime{};
	LARGE_INTEGER m_duckStartTime{};
	LARGE_INTEGER m_playbackT0{};
	LARGE_INTEGER m_perfFreq{};
	// Hangtime: holds mic open briefly after last track finishes, waiting for next sound
	uint32_t m_hangtimeClientId = 0;   // clientId of last-finished track, 0 if not in hangtime
	LARGE_INTEGER m_hangtimeStartQPC{};

	// Watchdog state (monitors network, sizebuf, and connection sanity during playback)
	struct WatchdogState
	{
		LARGE_INTEGER stateNot5StartTime{};
		LARGE_INTEGER unflushedStartTime{};
		LARGE_INTEGER steamIdZeroStartTime{};
	};
	WatchdogState m_watchdog;
	std::atomic<bool> m_bResetWatchdog{ false };

	// Panic State
	std::atomic<bool> m_bPanic{ false };
	std::atomic<bool> m_bPanicRecoverable{ false };
	char m_panicReason[256]{};
	LARGE_INTEGER m_recoverHealthySince{};

	// Voice Referee State
	std::atomic<bool> m_bUserTalking{ false };
	std::atomic<bool> m_bPluginTransmitting{ false };
	std::atomic<bool> m_bDucked{ false };
	LARGE_INTEGER m_rxLastCounter{};

	// 0 means no pending resume (either not ducked, or key still held).
	std::atomic<uint64_t> m_duckResumeQPC{ 0 };

	mutable std::atomic<uint64_t> m_cachedSteamId{ 0 };

	// Audio pipeline state
	std::unique_ptr<VoiceTrack> m_pCurrentTrack;
	std::vector<std::unique_ptr<VoiceTrack>> m_queue;
	std::vector<std::unique_ptr<VoiceTrack>> m_pausedStack;
	StreamingSession m_stream;
	LARGE_INTEGER m_streamLastPushQPC{};

	std::vector<IVoiceEventListener*> m_listeners;
	bool m_injectedLastFrame = false;

	VoiceTelemetry m_stats;
};

#endif // VOICE_CORE_H

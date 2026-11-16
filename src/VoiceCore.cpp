#include "VoiceCore.h"
#include "VoiceUtils.h"
#include "AudioDecoder.h"
#include "DebugLog.h"
#if __has_include(<opus.h>)
#include <opus.h>
#elif __has_include("opus/opus.h")
#include "opus/opus.h"
#elif __has_include("../include/opus/opus.h")
#include "../include/opus/opus.h"
#else
#include <opus.h>
#endif
#include <windows.h>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <memory>
#include <new>
#include <strsafe.h>
#include <metahook.h>
#include <cvardef.h>
#include <Interface/IFileSystem.h>

extern mh_interface_t* g_pInterface;
extern uint8_t* volatile* g_voiceSendData;
extern volatile uint32* g_voiceSendCursor;
extern volatile uint32* g_voiceSendMaxSize;
extern volatile int* g_clientStatePtr;
extern cvar_t* g_ve_duck_resume_ms; // registered in plugins.cpp; fallback to 150ms if nullptr
extern cvar_t* g_ve_allow_abs_paths;   // registered in plugins.cpp; controls absolute path access
extern cvar_t* g_ve_debug;

void SafeConPrintf(const char* fmt, ...);
#define VE_DEBUG_LOG(...) do { if (g_ve_debug && g_ve_debug->value > 0.0f) SafeConPrintf(__VA_ARGS__); } while (0)

namespace
{
	constexpr uint8_t kClcVoiceData = 8;
	constexpr int kTargetSampleRate = 24000;
	constexpr int kOpusFrameSamples = 480;       // 20ms at 24kHz
	constexpr uint64_t kFrameDurationUs = 20000;  // One Opus frame
	// The engine packs everything Steam GetVoice returned since the previous call
	// into one voice block. Observed blocks carry 1..8 frames (avg 2.5) at a ~50ms
	// Voice_Send cadence; keep the same upper bound for injected blocks.
	constexpr size_t kMaxFramesPerVoicePacket = 8;
	constexpr uint32_t kVoiceSendExpectedMaxSize = 0xFFCF;
	constexpr uint32_t kMaxSafeVoiceBuffer = 2048;
	constexpr uint32_t kMaxInjectBytesPerFrame = 1024;

	struct OpusEncoderDeleter
	{
		void operator()(OpusEncoder* e) const { if (e) opus_encoder_destroy(e); }
	};
	using OpusEncoderPtr = std::unique_ptr<OpusEncoder, OpusEncoderDeleter>;

	OpusEncoderPtr CreateVoiceEncoder()
	{
		int err = 0;
		OpusEncoderPtr enc(opus_encoder_create(kTargetSampleRate, 1, OPUS_APPLICATION_VOIP, &err));
		if (err != OPUS_OK || !enc)
			return nullptr;
		opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(32000));
		opus_encoder_ctl(enc.get(), OPUS_SET_COMPLEXITY(5));
		opus_encoder_ctl(enc.get(), OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
		return enc;
	}

	uint32_t CalculateCrc32(const uint8_t* data, size_t length)
	{
		uint32_t c = 0xFFFFFFFF;
		for (size_t i = 0; i < length; ++i)
		{
			c ^= data[i];
			for (int k = 0; k < 8; ++k)
			{
				c = (c >> 1) ^ (0xEDB88320 & -(static_cast<int32_t>(c & 1)));
			}
		}
		return ~c;
	}

	bool FileExists(const std::string& path)
	{
		DWORD attr = GetFileAttributesA(path.c_str());
		return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
	}

	// 2nd-order Butterworth low-pass filter to prevent aliasing when downsampling
	void ApplyLowPassFilter(std::vector<float>& samples, int sampleRate, float cutoffHz)
	{
		if (sampleRate <= 0 || samples.empty() || cutoffHz >= sampleRate * 0.5f)
			return;

		const double pi = 3.14159265358979323846;
		double ita = 1.0 / std::tan(pi * cutoffHz / sampleRate);
		double q = std::sqrt(2.0);
		double b0 = 1.0 / (1.0 + q * ita + ita * ita);
		double b1 = 2.0 * b0;
		double b2 = b0;
		double a1 = 2.0 * (ita * ita - 1.0) * b0;
		double a2 = -(1.0 - q * ita + ita * ita) * b0;

		double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		for (float& s : samples)
		{
			double in = s;
			double out = b0 * in + b1 * x1 + b2 * x2 + a1 * y1 + a2 * y2;
			x2 = x1;
			x1 = in;
			y2 = y1;
			y1 = out;
			s = static_cast<float>(out);
		}
	}

	// Produces a valid comfort-noise/silence frame so a single failed encode does
	// not leave a 20ms hole in the timeline.
	bool EncodeSilenceFrame(OpusEncoder* enc, std::vector<uint8_t>& out)
	{
		if (!enc)
			return false;
		int16_t silence[kOpusFrameSamples] = {};
		uint8_t buf[256];
		const int len = opus_encode(enc, silence, kOpusFrameSamples, buf, sizeof(buf));
		if (len <= 0)
			return false;
		out.assign(buf, buf + len);
		return true;
	}

	// Shared by file and raw-PCM paths. Uses its own dedicated encoder, so it needs no lock
	// and cannot disturb the encoder state of a live stream.
	bool EncodeMonoToChunks(std::vector<float>& mono, int sampleRate, const VoicePlayParams& params,
		std::vector<OpusChunk>& outChunks)
	{
		if (mono.empty() || sampleRate <= 0)
			return false;

		OpusEncoderPtr enc = CreateVoiceEncoder();
		if (!enc)
			return false;

		float pitch = params.pitch;
		if (!std::isfinite(pitch) || pitch <= 0.05f) pitch = 1.0f;
		pitch = std::clamp(pitch, 0.5f, 2.0f);
		float volume = std::isfinite(params.volume) ? std::clamp(params.volume, 0.0f, 2.0f) : 1.0f;

		// Anti-aliasing low-pass filter: apply whenever effective resampling step > 1.0
		// (due to higher source sample rate and/or pitch upshift > 1.0)
		const double step = static_cast<double>(sampleRate) * pitch / kTargetSampleRate;
		float effectiveCutoff = (kTargetSampleRate * 0.48f) / pitch;
		if (step > 1.0 && effectiveCutoff < sampleRate * 0.5f)
		{
			ApplyLowPassFilter(mono, sampleRate, effectiveCutoff);
		}
		size_t outCount = static_cast<size_t>(mono.size() / step);
		if (outCount == 0)
			return false;

		constexpr size_t kMaxEncodedSamples = static_cast<size_t>(kTargetSampleRate) * 600; // 10 minutes max at 24kHz
		if (outCount > kMaxEncodedSamples)
		{
			outCount = kMaxEncodedSamples;
		}

		std::vector<int16_t> pcm(outCount);
		double srcPos = 0.0;
		for (size_t i = 0; i < outCount; ++i)
		{
			const size_t i0 = static_cast<size_t>(srcPos);
			const size_t i1 = (i0 + 1 < mono.size()) ? i0 + 1 : i0;
			const float frac = static_cast<float>(srcPos - i0);
			float s = (mono[i0] + frac * (mono[i1] - mono[i0])) * volume;
			s = VoiceCore::SoftLimit(s);
			int32_t v = static_cast<int32_t>(s * 32767.0f);
			pcm[i] = static_cast<int16_t>(std::clamp(v, -32768, 32767));
			srcPos += step;
		}

		// Free large mono float buffer immediately after resampling to minimize memory pressure in 32-bit address space
		mono.clear();
		mono.shrink_to_fit();

		// No pre-roll/post-roll: silence here was a real gap between queued tracks.
		uint64_t dueUs = 0;

		size_t off = 0;
		while (off < pcm.size())
		{
			int16_t f1[kOpusFrameSamples] = {};
			size_t rem = pcm.size() - off;
			const size_t take1 = std::min<size_t>(rem, kOpusFrameSamples);
			memcpy(f1, pcm.data() + off, take1 * sizeof(int16_t));
			off += take1;

			uint8_t o1[512];
			const int len1 = opus_encode(enc.get(), f1, kOpusFrameSamples, o1, sizeof(o1));
			if (len1 > 0)
			{
				OpusChunk c;
				c.frame.assign(o1, o1 + len1);
				c.dueUs = dueUs;
				outChunks.push_back(std::move(c));
			}
			else
			{
				OpusChunk c;
				if (EncodeSilenceFrame(enc.get(), c.frame))
				{
					c.dueUs = dueUs;
					outChunks.push_back(std::move(c));
				}
			}
			dueUs += kFrameDurationUs;
		}

		return !outChunks.empty();
	}
}

VoiceCore& VoiceCore::Get()
{
	static VoiceCore instance;
	return instance;
}

VoiceCore::VoiceCore()
{
	InitializeCriticalSection(&m_lock);
	QueryPerformanceFrequency(&m_perfFreq);
}

VoiceCore::~VoiceCore()
{
	Shutdown();
	DeleteCriticalSection(&m_lock);
}

bool VoiceCore::Init()
{
	EnterCriticalSection(&m_lock);
	if (m_bInitialized)
	{
		LeaveCriticalSection(&m_lock);
		return true;
	}

	OpusEncoderPtr enc = CreateVoiceEncoder();
	if (!enc)
	{
		LeaveCriticalSection(&m_lock);
		return false;
	}

	m_pOpusEncoder = enc.release();
	m_bInitialized = true;
	LeaveCriticalSection(&m_lock);
	return true;
}

void VoiceCore::Shutdown()
{
	StopAll();

	EnterCriticalSection(&m_lock);
	m_listeners.clear();

	if (m_pOpusEncoder)
	{
		opus_encoder_destroy(m_pOpusEncoder);
		m_pOpusEncoder = nullptr;
	}

	m_stream.active = false;
	m_stream.ownerId = 0;
	m_registeredClients.clear();
	m_cachedSteamId.store(0, std::memory_order_relaxed);
	m_watchdog = {};
	m_bInitialized = false;
	LeaveCriticalSection(&m_lock);
}

uint32_t VoiceCore::RegisterClient(const char* clientName, const VoiceClientParams& params)
{
	EnterCriticalSection(&m_lock);
	if (m_registeredClients.size() >= 254)
	{
		LeaveCriticalSection(&m_lock);
		DebuLog("RegisterClient: failed, client registry full (254 clients)");
		return 0;
	}

	uint32_t candidate = m_nextClientId;
	for (int i = 0; i < 254; ++i)
	{
		if (candidate == 0 || candidate > 254)
			candidate = 1;

		if (m_registeredClients.find(candidate) == m_registeredClients.end() && !HasClientTracksLocked(candidate))
		{
			m_nextClientId = candidate + 1;
			if (m_nextClientId > 254)
				m_nextClientId = 1;
			m_registeredClients[candidate] = clientName ? clientName : "Client";
			m_clientParams[candidate] = params;
			LeaveCriticalSection(&m_lock);
			DebuLog("RegisterClient: registered id=%u name='%s' hangtimeMs=%u", candidate, clientName ? clientName : "Client", params.hangtimeMs);
			return candidate;
		}
		candidate++;
	}

	LeaveCriticalSection(&m_lock);
	return 0;
}

bool VoiceCore::HasClientTracksLocked(uint32_t clientId) const
{
	if (clientId == 0)
		return false;

	if (m_pCurrentTrack && m_pCurrentTrack->ownerId == clientId)
		return true;
	for (const auto& t : m_queue)
	{
		if (t && t->ownerId == clientId)
			return true;
	}
	for (const auto& t : m_pausedStack)
	{
		if (t && t->ownerId == clientId)
			return true;
	}
	if (m_stream.active && m_stream.ownerId == clientId)
		return true;
	return false;
}

void VoiceCore::UnregisterClient(uint32_t clientId, bool stopTracks)
{
	if (clientId == 0)
		return;
	DebuLog("UnregisterClient: unregistering id=%u (stopTracks=%d)", clientId, stopTracks);
	if (stopTracks)
		StopClient(clientId);
	else
		CloseStream(clientId);

	EnterCriticalSection(&m_lock);
	m_registeredClients.erase(clientId);
	m_clientParams.erase(clientId);
	LeaveCriticalSection(&m_lock);
}

// Invariant: Soft-knee limiter with hyperbolic tangent saturation.
// Below threshold (thresh = 0.707, -3 dBFS), audio passes through 100% bit-exact and uncolored.
// Above threshold, excess signal is smoothly mapped via tanh to asymptote asymptotically at 1.0 (0 dBFS),
// entirely eliminating harsh digital clipping and inter-sample peaks without full-frame dynamic compression artifacts.
float VoiceCore::SoftLimit(float x)
{
	return VoiceUtils::SoftLimit(x);
}

bool VoiceCore::IsPathSafe(const char* path, bool isFromPipe)
{
	const bool allowAbs = (g_ve_allow_abs_paths && g_ve_allow_abs_paths->value != 0.0f);
	const bool safe = VoiceUtils::IsPathSafe(path, isFromPipe, allowAbs);
	if (!safe)
	{
		DebuLog("IsPathSafe: REJECTED '%s' (isFromPipe=%d allowAbs=%d)", path ? path : "(null)", isFromPipe, allowAbs);
	}
	else
	{
		DebuLog("IsPathSafe: ACCEPTED '%s'", path);
	}
	return safe;
}

std::string VoiceCore::ResolveSoundPath(const char* relativePath, bool isFromPipe)
{
	if (!IsPathSafe(relativePath, isFromPipe))
	{
		DebuLog("ResolveSoundPath: IsPathSafe check failed for '%s'", relativePath ? relativePath : "(null)");
		return "";
	}

	std::string sub = relativePath;
	for (char& c : sub)
	{
		if (c == '\\')
			c = '/';
	}

	// Check for direct absolute path (already vetted by IsPathSafe)
	if (sub.size() >= 3 && isalpha(static_cast<unsigned char>(sub[0])) && sub[1] == ':' && sub[2] == '/')
	{
		DWORD attrs = GetFileAttributesA(sub.c_str());
		if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
		{
			DebuLog("ResolveSoundPath: absolute path verified on disk '%s'", sub.c_str());
			return sub;
		}
		DebuLog("ResolveSoundPath: absolute path not found on disk '%s'", sub.c_str());
		return "";
	}

	if (sub.rfind("sound/", 0) == 0)
	{
		sub = sub.substr(6);
	}

	// 1. Prefer engine's IFileSystem to respect mounting order and game overrides
	IFileSystem* pFS = g_pInterface ? g_pInterface->FileSystem : nullptr;
	if (!pFS && g_pInterface)
		pFS = reinterpret_cast<IFileSystem*>(g_pInterface->FileSystem_HL25);

	if (pFS)
	{
		std::string fsRelative = "sound/" + sub;
		if (pFS->FileExists(fsRelative.c_str()))
		{
			char localPath[MAX_PATH] = {};
			if (pFS->GetLocalPath(fsRelative.c_str(), localPath, sizeof(localPath)) && localPath[0])
			{
				DebuLog("ResolveSoundPath: IFileSystem resolved to local path '%s'", localPath);
				return localPath;
			}
			DebuLog("ResolveSoundPath: IFileSystem found relative '%s'", fsRelative.c_str());
			return fsRelative;
		}
	}

	// 2. Fallback search order: User Addon -> Server Downloads -> Base Game -> Engine Valve
	static const char* kSearchDirs[] = {
		"svencoop_addon/sound/",
		"svencoop_downloads/sound/",
		"svencoop/sound/",
		"valve/sound/"
	};

	for (const char* prefix : kSearchDirs)
	{
		std::string candidate = std::string(prefix) + sub;
		if (FileExists(candidate))
		{
			DebuLog("ResolveSoundPath: fallback search matched '%s'", candidate.c_str());
			return candidate;
		}
	}

	DebuLog("ResolveSoundPath: sound file NOT FOUND '%s'", relativePath);
	return "";
}

uint64_t VoiceCore::QuerySteamID() const
{
	uint64_t cached = m_cachedSteamId.load(std::memory_order_relaxed);
	if (cached != 0)
		return cached;

	HMODULE hSteamApi = GetModuleHandleA("steam_api.dll");
	if (hSteamApi)
	{
		using FnSteamAPI_IsSteamRunning = bool(*)();
		using FnGetHSteamUser = int32_t(*)();
		using FnFindOrCreateUserInterface = void*(*)(int32_t, const char*);
		using FnSteamAPI_ISteamUser_GetSteamID = uint64_t(*)(void*);

		auto pfnIsSteamRunning = reinterpret_cast<FnSteamAPI_IsSteamRunning>(GetProcAddress(hSteamApi, "SteamAPI_IsSteamRunning"));
		if (pfnIsSteamRunning && !pfnIsSteamRunning())
		{
			return 0;
		}

		auto pfnGetHSteamUser = reinterpret_cast<FnGetHSteamUser>(GetProcAddress(hSteamApi, "SteamAPI_GetHSteamUser"));
		auto pfnFindOrCreateUserInterface = reinterpret_cast<FnFindOrCreateUserInterface>(GetProcAddress(hSteamApi, "SteamInternal_FindOrCreateUserInterface"));
		auto pfnGetSteamID = reinterpret_cast<FnSteamAPI_ISteamUser_GetSteamID>(GetProcAddress(hSteamApi, "SteamAPI_ISteamUser_GetSteamID"));

		if (pfnGetHSteamUser && pfnFindOrCreateUserInterface && pfnGetSteamID)
		{
			int32_t hUser = pfnGetHSteamUser();
			if (hUser)
			{
				void* pSteamUser = pfnFindOrCreateUserInterface(hUser, "SteamUser020");
				if (pSteamUser)
				{
					uint64_t id = pfnGetSteamID(pSteamUser);
					if (id != 0)
					{
						m_cachedSteamId.store(id, std::memory_order_relaxed);
						return id;
					}
				}
			}
		}
	}

	if (gEngfuncs.GetLocalPlayer && gEngfuncs.pfnGetPlayerInfo)
	{
		cl_entity_t* localEnt = gEngfuncs.GetLocalPlayer();
		if (localEnt && localEnt->player != 0 && localEnt->index > 0)
		{
			hud_player_info_t localInfo = {};
			gEngfuncs.pfnGetPlayerInfo(localEnt->index, &localInfo);
			if (localInfo.m_nSteamID != 0)
			{
				m_cachedSteamId.store(localInfo.m_nSteamID, std::memory_order_relaxed);
				return localInfo.m_nSteamID;
			}
		}
	}

	return 0;
}

void VoiceCore::OnVoicePacketObserved(const uint8_t* data, size_t size)
{
	if (!data || size < 22 || data[8] != 11 || data[11] != 6)
		return;

	const size_t payloadLen = static_cast<size_t>(data[12]) | (static_cast<size_t>(data[13]) << 8);
	if (14 + payloadLen + 4 != size)
		return;

	const size_t end = 14 + payloadLen;
	size_t pos = 14;
	uint16_t lastSeq = 0;
	while (pos + 4 <= end)
	{
		uint16_t frameLen = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
		uint16_t frameSeq = static_cast<uint16_t>(data[pos + 2] | (data[pos + 3] << 8));
		pos += 4;
		if (pos + frameLen > end)
			break;
		lastSeq = frameSeq;
		pos += frameLen;
	}

	if (lastSeq > 0)
	{
		m_globalSequence.store(static_cast<uint16_t>(lastSeq + 1), std::memory_order_relaxed);
	}
}

bool VoiceCore::IsStreamingActive(uint32_t clientId) const
{
	EnterCriticalSection(&m_lock);
	const bool r = m_stream.active && (clientId == 0 || m_stream.ownerId == clientId);
	LeaveCriticalSection(&m_lock);
	return r;
}

uint32_t VoiceCore::GetStreamOwner() const
{
	EnterCriticalSection(&m_lock);
	const uint32_t r = m_stream.active ? m_stream.ownerId : 0;
	LeaveCriticalSection(&m_lock);
	return r;
}

bool VoiceCore::IsCurrentTrackOwner(uint32_t clientId) const
{
	if (clientId == 0)
		return true;

	EnterCriticalSection(&m_lock);
	const bool r = (m_pCurrentTrack && m_pCurrentTrack->ownerId == clientId);
	LeaveCriticalSection(&m_lock);
	return r;
}

bool VoiceCore::EncodeFileToChunks(const char* fullPath, const VoicePlayParams& params,
	std::vector<OpusChunk>& outChunks)
{
	DecodedAudio decoded;
	if (!AudioDecoder::DecodeFile(fullPath, decoded))
		return false;
	return EncodeMonoToChunks(decoded.monoSamples, decoded.sampleRate, params, outChunks);
}

bool VoiceCore::EncodePcmToChunks(const int16_t* pcm, size_t samples, int sampleRate,
	const VoicePlayParams& params, std::vector<OpusChunk>& outChunks)
{
	if (!pcm || samples == 0 || sampleRate <= 0)
		return false;
	std::vector<float> mono(samples);
	for (size_t i = 0; i < samples; ++i)
		mono[i] = static_cast<float>(pcm[i]) / 32768.0f;
	return EncodeMonoToChunks(mono, sampleRate, params, outChunks);
}

uint32_t VoiceCore::InsertTrackLocked(std::unique_ptr<VoiceTrack> track, bool& startedNow, std::string& startedName,
	bool& preemptedStream, uint32_t& preemptedStreamId, std::string& preemptedStreamName,
	bool& droppedPaused, uint32_t& droppedPausedId, std::string& droppedPausedName,
	bool& notifyState, VoicePlaybackState& stateToNotify)
{
	constexpr size_t kMaxQueueTracks = 128;
	constexpr size_t kMaxQueuePerClient = 64;
	if (m_queue.size() >= kMaxQueueTracks)
		return 0;

	// Per-client quota check
	size_t clientCount = 0;
	for (const auto& t : m_queue)
	{
		if (t && t->ownerId == track->ownerId)
			clientCount++;
	}
	if (clientCount >= kMaxQueuePerClient)
	{
		DebuLog("InsertTrackLocked: client %u exceeded queue quota (%zu >= %zu)", track->ownerId, clientCount, kMaxQueuePerClient);
		return 0;
	}

	const uint32_t id = track->trackId;
	const uint32_t callerId = track->callerTrackId;
	const std::string trkName = track->name;
	const VoicePriority prio = track->params.priority;

	auto startNow = [&]()
	{
		startedNow = true;
		startedName = track->name;
		// Cancel any active hangtime — new track arrived, session continues seamlessly
		m_hangtimeClientId = 0;
		m_hangtimeStartQPC.QuadPart = 0;
		const bool allowDuck = track->params.allowDucking;
		m_pCurrentTrack = std::move(track);
		QueryPerformanceCounter(&m_playbackT0);
		if (allowDuck && m_bUserTalking.load(std::memory_order_relaxed))
		{
			notifyState = SetStateLocked(VoicePlaybackState::DUCKED, stateToNotify);
			QueryPerformanceCounter(&m_duckStartTime);
		}
		else
		{
			notifyState = SetStateLocked(VoicePlaybackState::PLAYING, stateToNotify);
		}
		m_bPluginTransmitting.store(true, std::memory_order_relaxed);
	};

	if (prio == VoicePriority::URGENT && m_pCurrentTrack)
	{
		if (m_pCurrentTrack->isStream)
		{
			// Live stream cannot be frozen in paused stack: terminate it cleanly
			m_stream.active = false;
			m_stream.ownerId = 0;
			m_stream.pcmAccumulator.clear();
			m_stream.resamplePhase = 0.0;
			preemptedStream = true;
			preemptedStreamId = m_pCurrentTrack->GetEventTrackId();
			preemptedStreamName = m_pCurrentTrack->name;
			m_pCurrentTrack.reset();
		}
		else
		{
			constexpr size_t kMaxPausedStack = 16;
			if (m_pausedStack.size() >= kMaxPausedStack)
			{
				droppedPaused = true;
				droppedPausedId = m_pausedStack.front()->GetEventTrackId();
				droppedPausedName = m_pausedStack.front()->name;
				m_pausedStack.erase(m_pausedStack.begin());
			}
			m_pausedStack.push_back(std::move(m_pCurrentTrack));
		}
		startNow();
	}
	else if (!m_pCurrentTrack)
	{
		startNow();
	}
	else
	{
		const uint64_t incomingOrder = track->submissionOrder;
		auto it = m_queue.begin();
		while (it != m_queue.end())
		{
			const VoicePriority itPrio = (*it)->params.priority;
			if (itPrio > prio)
			{
				++it;
				continue;
			}
			if (itPrio == prio && (*it)->submissionOrder <= incomingOrder)
			{
				++it;
				continue;
			}
			break;
		}
		m_queue.insert(it, std::move(track));
	}

	if (startedNow)
	{
		VE_DEBUG_LOG("[VoiceExtension] InsertTrack: started id=0x%08X (callerId=%u, name='%s')\n",
			id, callerId, startedName.c_str());
	}
	else
	{
		VE_DEBUG_LOG("[VoiceExtension] InsertTrack: queued id=0x%08X (callerId=%u, name='%s', queueSize=%zu, currentTrack='%s')\n",
			id, callerId, trkName.c_str(), m_queue.size(),
			m_pCurrentTrack ? m_pCurrentTrack->name.c_str() : "none");
	}

	return id;
}

uint32_t VoiceCore::EnqueueFile(const char* relativePath, const VoicePlayParams& params, bool isFromPipe, uint32_t clientId)
{
	if (m_bPanic.load(std::memory_order_relaxed) || !Init())
		return 0;

	const std::string resolved = ResolveSoundPath(relativePath, isFromPipe);
	if (resolved.empty())
	{
		SafeConPrintf("[VoiceExtension] EnqueueFile: ResolveSoundPath failed for '%s'\n", relativePath ? relativePath : "(null)");
		return 0;
	}

	const uint64_t submissionOrder = m_submissionCounter.fetch_add(1, std::memory_order_relaxed);

	std::unique_ptr<VoiceTrack> track;
	try
	{
		track = std::make_unique<VoiceTrack>();
		uint32_t seq = m_nextTrackSeq.fetch_add(1) & 0x00FFFFFF;
		if (seq == 0) seq = m_nextTrackSeq.fetch_add(1) & 0x00FFFFFF;
		track->trackId = ((clientId & 0xFF) << 24) | seq;
		track->ownerId = clientId;
		track->callerTrackId = params.trackId;
		track->name = resolved;
		track->params = params;
		track->isStream = false;
		track->submissionOrder = submissionOrder;

		// Decode + resample + Opus run WITHOUT m_lock, so the game thread never waits on them.
		if (!EncodeFileToChunks(resolved.c_str(), params, track->chunks))
		{
			SafeConPrintf("[VoiceExtension] EnqueueFile: EncodeFileToChunks failed for '%s'\n", resolved.c_str());
			return 0;
		}

		VE_DEBUG_LOG("[VoiceExtension] EnqueueFile: resolved='%s', chunks=%zu, clientId=%u, callerTrackId=%u\n",
			resolved.c_str(), track->chunks.size(), clientId, params.trackId);
	}
	catch (...)
	{
		return 0; // 32-bit process: OOM/invalid input must not take the game down
	}

	bool startedNow = false;
	std::string startedName;
	bool preemptedStream = false;
	uint32_t preemptedStreamId = 0;
	std::string preemptedStreamName;
	bool droppedPaused = false;
	uint32_t droppedPausedId = 0;
	std::string droppedPausedName;
	bool notifyState = false;
	VoicePlaybackState stateToNotify = VoicePlaybackState::IDLE;

	EnterCriticalSection(&m_lock);
	if (m_bPanic.load(std::memory_order_relaxed) || !m_bInitialized)
	{
		LeaveCriticalSection(&m_lock);
		return 0;
	}
	uint32_t id = 0;
	try
	{
		id = InsertTrackLocked(std::move(track), startedNow, startedName,
			preemptedStream, preemptedStreamId, preemptedStreamName,
			droppedPaused, droppedPausedId, droppedPausedName,
			notifyState, stateToNotify);
	}
	catch (...)
	{
		LeaveCriticalSection(&m_lock);
		return 0;
	}
	LeaveCriticalSection(&m_lock);

	if (preemptedStream)
	{
		NotifyTrackFinished(preemptedStreamId, preemptedStreamName.c_str());
	}
	if (droppedPaused)
	{
		NotifyTrackFinished(droppedPausedId, droppedPausedName.c_str());
	}
	if (startedNow)
	{
		NotifyTrackStarted(params.trackId ? params.trackId : id, startedName.c_str());
	}
	if (notifyState)
	{
		NotifyStateChanged(stateToNotify);
	}
	return id;
}

uint32_t VoiceCore::EnqueuePcm(const int16_t* pcm, size_t samples, int sampleRate, const VoicePlayParams& params, uint32_t clientId)
{
	if (!pcm || samples == 0 || sampleRate <= 0)
		return 0;
	// 32-bit process: the float staging buffer is samples * sizeof(float), so cap
	// it at 64 MB even for high-rate PCM (10 minutes remains the time limit).
	constexpr size_t kMaxPcmBytes = 64ull * 1024 * 1024;
	const size_t kMaxPcmSamples = (std::min)(static_cast<size_t>(sampleRate) * 600,
		kMaxPcmBytes / sizeof(float));
	if (samples > kMaxPcmSamples)
	{
		DebuLog("EnqueuePcm: clamping PCM samples from %zu to %zu (10 minutes max)", samples, kMaxPcmSamples);
		samples = kMaxPcmSamples;
	}
	if (m_bPanic.load(std::memory_order_relaxed) || !Init())
		return 0;

	const uint64_t submissionOrder = m_submissionCounter.fetch_add(1, std::memory_order_relaxed);

	std::unique_ptr<VoiceTrack> track;
	try
	{
		track = std::make_unique<VoiceTrack>();
		uint32_t seq = m_nextTrackSeq.fetch_add(1) & 0x00FFFFFF;
		if (seq == 0) seq = m_nextTrackSeq.fetch_add(1) & 0x00FFFFFF;
		track->trackId = ((clientId & 0xFF) << 24) | seq;
		track->ownerId = clientId;
		track->callerTrackId = params.trackId;
		track->name = "[In-Memory PCM]";
		track->params = params;
		track->isStream = false;
		track->submissionOrder = submissionOrder;

		if (!EncodePcmToChunks(pcm, samples, sampleRate, params, track->chunks))
			return 0;
	}
	catch (...)
	{
		return 0;
	}

	bool startedNow = false;
	std::string startedName;
	bool preemptedStream = false;
	uint32_t preemptedStreamId = 0;
	std::string preemptedStreamName;
	bool droppedPaused = false;
	uint32_t droppedPausedId = 0;
	std::string droppedPausedName;
	bool notifyState = false;
	VoicePlaybackState stateToNotify = VoicePlaybackState::IDLE;

	EnterCriticalSection(&m_lock);
	if (m_bPanic.load(std::memory_order_relaxed) || !m_bInitialized)
	{
		LeaveCriticalSection(&m_lock);
		return 0;
	}
	uint32_t id = 0;
	try
	{
		id = InsertTrackLocked(std::move(track), startedNow, startedName,
			preemptedStream, preemptedStreamId, preemptedStreamName,
			droppedPaused, droppedPausedId, droppedPausedName,
			notifyState, stateToNotify);
	}
	catch (...)
	{
		LeaveCriticalSection(&m_lock);
		return 0;
	}
	LeaveCriticalSection(&m_lock);

	if (preemptedStream)
	{
		NotifyTrackFinished(preemptedStreamId, preemptedStreamName.c_str());
	}
	if (droppedPaused)
	{
		NotifyTrackFinished(droppedPausedId, droppedPausedName.c_str());
	}
	if (startedNow)
	{
		NotifyTrackStarted(params.trackId ? params.trackId : id, startedName.c_str());
	}
	if (notifyState)
	{
		NotifyStateChanged(stateToNotify);
	}
	return id;
}

void VoiceCore::DrainStreamAccumulatorLocked(bool flushTail)
{
	auto& acc = m_stream.pcmAccumulator;
	if (!m_pOpusEncoder || !m_pCurrentTrack || !m_pCurrentTrack->isStream)
	{
		acc.clear();
		return;
	}

	const size_t packet = static_cast<size_t>(kOpusFrameSamples);
	if (flushTail && !acc.empty() && (acc.size() % packet) != 0)
		acc.resize(((acc.size() / packet) + 1) * packet, 0); // pad tail so speech is not clipped

	size_t offset = 0;
	while (acc.size() - offset >= packet)
	{
		const int16_t* f1 = acc.data() + offset;
		uint8_t o1[512];
		const int len1 = opus_encode(m_pOpusEncoder, f1, kOpusFrameSamples, o1, sizeof(o1));

		OpusChunk c;
		if (len1 > 0)
		{
			c.frame.assign(o1, o1 + len1);
		}
		else if (!EncodeSilenceFrame(m_pOpusEncoder, c.frame))
		{
			// Keep the stream clock advancing if even the silence fallback fails.
			m_stream.nextDueUs += kFrameDurationUs;
			offset += packet;
			continue;
		}

		c.dueUs = m_stream.nextDueUs;
		m_stream.nextDueUs += kFrameDurationUs;
		m_pCurrentTrack->chunks.push_back(std::move(c));
		offset += packet;
	}
	acc.erase(acc.begin(), acc.begin() + offset);
}

void VoiceCore::AdvanceTrackLocked(bool& notifyNextStarted, uint32_t& nextStartedId, std::string& nextStartedName,
	bool& notifyState, VoicePlaybackState& stateToNotify)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	while (!m_pausedStack.empty() && m_pausedStack.back()->isStream)
	{
		m_pausedStack.pop_back();
	}

	// Interrupted urgent tracks take precedence over regular queued tracks
	if (!m_pausedStack.empty())
	{
		m_pCurrentTrack = std::move(m_pausedStack.back());
		m_pausedStack.pop_back();

		uint64_t currentDueUs = (m_pCurrentTrack->currentChunk < m_pCurrentTrack->chunks.size())
			? m_pCurrentTrack->chunks[m_pCurrentTrack->currentChunk].dueUs : 0;
		m_playbackT0.QuadPart = now.QuadPart - static_cast<LONGLONG>(currentDueUs * m_perfFreq.QuadPart / 1000000ull);

		const bool isTalking = m_bUserTalking.load(std::memory_order_relaxed);
		const bool allowDuck = m_pCurrentTrack->params.allowDucking;
		if (allowDuck && isTalking)
		{
			notifyState = SetStateLocked(VoicePlaybackState::DUCKED, stateToNotify);
			QueryPerformanceCounter(&m_duckStartTime);
		}
		else
		{
			notifyState = SetStateLocked(VoicePlaybackState::PLAYING, stateToNotify);
		}
		m_bPluginTransmitting.store(true, std::memory_order_relaxed);

		notifyNextStarted = true;
		nextStartedId = m_pCurrentTrack->GetEventTrackId();
		nextStartedName = m_pCurrentTrack->name;
		VE_DEBUG_LOG("[VoiceExtension] AdvanceTrack: resumed paused track id=0x%08X (eventTrackId=%u, name='%s')\n",
			m_pCurrentTrack->trackId, nextStartedId, nextStartedName.c_str());
	}
	else if (!m_queue.empty())
	{
		m_pCurrentTrack = std::move(m_queue.front());
		m_queue.erase(m_queue.begin());
		m_playbackT0 = now;

		const bool isTalking = m_bUserTalking.load(std::memory_order_relaxed);
		const bool allowDuck = m_pCurrentTrack->params.allowDucking;
		if (allowDuck && isTalking)
		{
			notifyState = SetStateLocked(VoicePlaybackState::DUCKED, stateToNotify);
			QueryPerformanceCounter(&m_duckStartTime);
		}
		else
		{
			notifyState = SetStateLocked(VoicePlaybackState::PLAYING, stateToNotify);
		}
		m_bPluginTransmitting.store(true, std::memory_order_relaxed);

		notifyNextStarted = true;
		nextStartedId = m_pCurrentTrack->GetEventTrackId();
		nextStartedName = m_pCurrentTrack->name;
		VE_DEBUG_LOG("[VoiceExtension] AdvanceTrack: popped next queued track id=0x%08X (eventTrackId=%u, name='%s')\n",
			m_pCurrentTrack->trackId, nextStartedId, nextStartedName.c_str());
	}
	else
	{
		m_bPluginTransmitting.store(false, std::memory_order_relaxed);
		m_bPaused.store(false, std::memory_order_relaxed);
		m_pauseStartTime.QuadPart = 0;
		notifyState = SetStateLocked(VoicePlaybackState::IDLE, stateToNotify);
		VE_DEBUG_LOG("[VoiceExtension] AdvanceTrack: queue empty, going IDLE\n");
	}
}

bool VoiceCore::OpenStream(int sampleRate, const VoicePlayParams& params, uint32_t clientId)
{
	if (m_bPanic.load(std::memory_order_relaxed) || !Init())
		return false;

	bool notifyStarted = false;
	uint32_t startedId = 0;
	std::string startedName;

	bool notifyPrevStreamFinished = false;
	uint32_t prevStreamId = 0;
	std::string prevStreamName;

	bool notifyDroppedPaused = false;
	uint32_t droppedPausedId = 0;
	std::string droppedPausedName;

	bool notifyState = false;
	VoicePlaybackState stateToNotify = VoicePlaybackState::IDLE;

	EnterCriticalSection(&m_lock);

	// Stream arbitration: only one client may hold the active stream session.
	if (m_stream.active)
	{
		if (m_stream.ownerId != clientId)
		{
			DebuLog("OpenStream: rejected, stream already owned by client %u (request from %u)", m_stream.ownerId, clientId);
			LeaveCriticalSection(&m_lock);
			return false;
		}
	}

	// Pre-empt current track only if the stream priority is equal or higher.
	// A PASSIVE stream must not interrupt an URGENT or NORMAL file track.
	if (m_pCurrentTrack)
	{
		if (params.priority >= m_pCurrentTrack->params.priority)
		{
			if (m_pCurrentTrack->isStream)
			{
				notifyPrevStreamFinished = true;
				prevStreamId = m_pCurrentTrack->GetEventTrackId();
				prevStreamName = m_pCurrentTrack->name;
				m_pCurrentTrack.reset();
			}
			else
			{
				constexpr size_t kMaxPausedStack = 16;
				if (m_pausedStack.size() >= kMaxPausedStack)
				{
					notifyDroppedPaused = true;
					droppedPausedId = m_pausedStack.front()->GetEventTrackId();
					droppedPausedName = m_pausedStack.front()->name;
					m_pausedStack.erase(m_pausedStack.begin());
				}
				m_pausedStack.push_back(std::move(m_pCurrentTrack));
			}
		}
		else
		{
			LeaveCriticalSection(&m_lock);
			return false;
		}
	}

	m_stream.active = true;
	m_stream.ownerId = clientId;
	m_stream.sourceSampleRate = (sampleRate > 0) ? sampleRate : kTargetSampleRate;
	m_stream.params = params;
	m_stream.pcmAccumulator.clear();
	m_stream.nextDueUs = 0;
	m_stream.resamplePhase = 0.0;

	if (m_pOpusEncoder)
	{
		opus_encoder_ctl(m_pOpusEncoder, OPUS_RESET_STATE);
	}
	QueryPerformanceCounter(&m_streamLastPushQPC);

	std::unique_ptr<VoiceTrack> newTrack;
	try
	{
		newTrack = std::make_unique<VoiceTrack>();
	}
	catch (...)
	{
		LeaveCriticalSection(&m_lock);
		return false;
	}
	m_pCurrentTrack = std::move(newTrack);
	uint32_t seq = m_nextTrackSeq.fetch_add(1) & 0x00FFFFFF;
	if (seq == 0) seq = m_nextTrackSeq.fetch_add(1) & 0x00FFFFFF;
	m_pCurrentTrack->trackId = ((clientId & 0xFF) << 24) | seq;
	m_pCurrentTrack->ownerId = clientId;
	m_pCurrentTrack->callerTrackId = params.trackId;
	m_pCurrentTrack->name = "[Live Streaming]";
	m_pCurrentTrack->params = params;
	m_pCurrentTrack->isStream = true;

	QueryPerformanceCounter(&m_playbackT0);
	const bool isTalking = m_bUserTalking.load(std::memory_order_relaxed);
	const bool allowDuck = params.allowDucking;
	if (allowDuck && isTalking)
	{
		notifyState = SetStateLocked(VoicePlaybackState::DUCKED, stateToNotify);
		QueryPerformanceCounter(&m_duckStartTime);
	}
	else
	{
		notifyState = SetStateLocked(VoicePlaybackState::PLAYING, stateToNotify);
	}
	m_bPluginTransmitting.store(true, std::memory_order_relaxed);

	notifyStarted = true;
	startedId = m_pCurrentTrack->GetEventTrackId();
	startedName = m_pCurrentTrack->name;

	LeaveCriticalSection(&m_lock);

	if (notifyPrevStreamFinished)
	{
		NotifyTrackFinished(prevStreamId, prevStreamName.c_str());
	}
	if (notifyDroppedPaused)
	{
		NotifyTrackFinished(droppedPausedId, droppedPausedName.c_str());
	}
	if (notifyStarted)
	{
		NotifyTrackStarted(startedId, startedName.c_str());
	}
	if (notifyState)
	{
		NotifyStateChanged(stateToNotify);
	}
	return true;
}

bool VoiceCore::PushStreamPcm(const int16_t* pcm, size_t samples, uint32_t clientId)
{
	if (!pcm || samples == 0)
		return false;

	EnterCriticalSection(&m_lock);
	if (!m_stream.active || m_stream.ownerId != clientId || !m_pCurrentTrack || !m_pCurrentTrack->isStream)
	{
		LeaveCriticalSection(&m_lock);
		return false;
	}

	// Limit backlog of chunks to prevent unbounded memory growth if caller floods stream PCM
	constexpr size_t kMaxStreamChunkBacklog = 500; // ~10 seconds of 20ms frames
	if (m_pCurrentTrack->chunks.size() > m_pCurrentTrack->currentChunk &&
		(m_pCurrentTrack->chunks.size() - m_pCurrentTrack->currentChunk) >= kMaxStreamChunkBacklog)
	{
		LeaveCriticalSection(&m_lock);
		return false;
	}

	// Clamp max samples per push to 5 seconds to bound processing time under m_lock
	const size_t kMaxPushSamples = static_cast<size_t>(m_stream.sourceSampleRate) * 5;
	if (samples > kMaxPushSamples)
	{
		samples = kMaxPushSamples;
	}

	QueryPerformanceCounter(&m_streamLastPushQPC);

	// Resample chunk with fractional phase continuity across boundaries
	double step = static_cast<double>(m_stream.sourceSampleRate) / static_cast<double>(kTargetSampleRate);
	double srcPos = m_stream.resamplePhase;

	while (srcPos < static_cast<double>(samples))
	{
		size_t idx0 = static_cast<size_t>(srcPos);
		size_t idx1 = (idx0 + 1 < samples) ? idx0 + 1 : idx0;
		float frac = static_cast<float>(srcPos - idx0);

		float s0 = static_cast<float>(pcm[idx0]) / 32768.0f;
		float s1 = static_cast<float>(pcm[idx1]) / 32768.0f;
		float sample = s0 + frac * (s1 - s0);
		sample *= m_stream.params.volume;
		sample = SoftLimit(sample);

		int32_t val = static_cast<int32_t>(sample * 32767.0f);
		if (val > 32767) val = 32767;
		if (val < -32768) val = -32768;
		m_stream.pcmAccumulator.push_back(static_cast<int16_t>(val));

		srcPos += step;
	}

	// Store fractional remainder for next chunk
	m_stream.resamplePhase = srcPos - static_cast<double>(samples);

	DrainStreamAccumulatorLocked(false);

	LeaveCriticalSection(&m_lock);
	return true;
}

void VoiceCore::CloseStream(uint32_t clientId)
{
	uint32_t finishedId = 0;
	std::string finishedName;
	bool notifyFinished = false;

	uint32_t nextStartedId = 0;
	std::string nextStartedName;
	bool notifyNextStarted = false;

	bool notifyState = false;
	VoicePlaybackState stateToNotify = VoicePlaybackState::IDLE;

	EnterCriticalSection(&m_lock);
	if (!m_stream.active || m_stream.ownerId != clientId)
	{
		LeaveCriticalSection(&m_lock);
		return;
	}

	m_stream.active = false;
	m_stream.ownerId = 0;
	DrainStreamAccumulatorLocked(true); // flush the partial tail packet before dropping state
	m_stream.resamplePhase = 0.0;

	// If the streaming track has already delivered all its chunks, retire it inline.
	if (m_pCurrentTrack && m_pCurrentTrack->isStream &&
	    m_pCurrentTrack->currentChunk >= m_pCurrentTrack->chunks.size())
	{
		notifyFinished = true;
		finishedId = m_pCurrentTrack->GetEventTrackId();
		finishedName = m_pCurrentTrack->name;
		m_pCurrentTrack.reset();

		AdvanceTrackLocked(notifyNextStarted, nextStartedId, nextStartedName, notifyState, stateToNotify);
	}

	LeaveCriticalSection(&m_lock);

	if (notifyFinished)
	{
		NotifyTrackFinished(finishedId, finishedName.c_str());
	}
	if (notifyNextStarted)
	{
		NotifyTrackStarted(nextStartedId, nextStartedName.c_str());
	}
	if (notifyState)
	{
		NotifyStateChanged(stateToNotify);
	}
}

bool VoiceCore::StopTrack(uint32_t trackId)
{
	if (trackId == 0)
		return false;

	uint32_t finishedId = 0;
	std::string finishedName;
	bool notifyFinished = false;

	uint32_t nextStartedId = 0;
	std::string nextStartedName;
	bool notifyNextStarted = false;

	bool notifyState = false;
	VoicePlaybackState stateToNotify = VoicePlaybackState::IDLE;

	EnterCriticalSection(&m_lock);

	auto Matches = [trackId](const std::unique_ptr<VoiceTrack>& t) {
		return t && t->trackId == trackId;
	};

	// 1. Current track
	if (Matches(m_pCurrentTrack))
	{
		notifyFinished = true;
		finishedId = m_pCurrentTrack->GetEventTrackId();
		finishedName = m_pCurrentTrack->name;
		if (m_pCurrentTrack->isStream)
		{
			m_stream.active = false;
			m_stream.ownerId = 0;
			m_stream.pcmAccumulator.clear();
			m_stream.resamplePhase = 0.0;
		}
		m_pCurrentTrack.reset();

		AdvanceTrackLocked(notifyNextStarted, nextStartedId, nextStartedName, notifyState, stateToNotify);
		LeaveCriticalSection(&m_lock);

		if (notifyFinished)
			NotifyTrackFinished(finishedId, finishedName.c_str());
		if (notifyNextStarted)
			NotifyTrackStarted(nextStartedId, nextStartedName.c_str());
		if (notifyState)
			NotifyStateChanged(stateToNotify);
		return true;
	}

	// 2. Waiting queue
	for (auto it = m_queue.begin(); it != m_queue.end(); ++it)
	{
		if (Matches(*it))
		{
			finishedId = (*it)->GetEventTrackId();
			finishedName = (*it)->name;
			m_queue.erase(it);
			LeaveCriticalSection(&m_lock);
			NotifyTrackFinished(finishedId, finishedName.c_str());
			return true;
		}
	}

	// 3. Paused stack
	for (auto it = m_pausedStack.begin(); it != m_pausedStack.end(); ++it)
	{
		if (Matches(*it))
		{
			finishedId = (*it)->GetEventTrackId();
			finishedName = (*it)->name;
			m_pausedStack.erase(it);
			LeaveCriticalSection(&m_lock);
			NotifyTrackFinished(finishedId, finishedName.c_str());
			return true;
		}
	}

	LeaveCriticalSection(&m_lock);
	return false;
}

void VoiceCore::StopClient(uint32_t clientId)
{
	if (clientId == 0)
		return;

	std::vector<std::pair<uint32_t, std::string>> tracksToNotify;
	bool notifyNextStarted = false;
	uint32_t nextStartedId = 0;
	std::string nextStartedName;
	bool notifyState = false;
	VoicePlaybackState stateToNotify = VoicePlaybackState::IDLE;

	EnterCriticalSection(&m_lock);

	if (m_stream.active && m_stream.ownerId == clientId)
	{
		m_stream.active = false;
		m_stream.ownerId = 0;
		m_stream.pcmAccumulator.clear();
		m_stream.resamplePhase = 0.0;
	}

	bool currentStopped = false;
	if (m_pCurrentTrack && m_pCurrentTrack->ownerId == clientId)
	{
		tracksToNotify.push_back({ m_pCurrentTrack->GetEventTrackId(), m_pCurrentTrack->name });
		m_pCurrentTrack.reset();
		currentStopped = true;
	}

	for (auto it = m_queue.begin(); it != m_queue.end(); )
	{
		if ((*it)->ownerId == clientId)
		{
			tracksToNotify.push_back({ (*it)->GetEventTrackId(), (*it)->name });
			it = m_queue.erase(it);
		}
		else
		{
			++it;
		}
	}

	for (auto it = m_pausedStack.begin(); it != m_pausedStack.end(); )
	{
		if ((*it)->ownerId == clientId)
		{
			tracksToNotify.push_back({ (*it)->GetEventTrackId(), (*it)->name });
			it = m_pausedStack.erase(it);
		}
		else
		{
			++it;
		}
	}

	if (currentStopped)
	{
		AdvanceTrackLocked(notifyNextStarted, nextStartedId, nextStartedName, notifyState, stateToNotify);
	}

	if (m_hangtimeClientId == clientId)
	{
		m_hangtimeClientId = 0;
		m_hangtimeStartQPC.QuadPart = 0;
	}

	LeaveCriticalSection(&m_lock);

	for (const auto& trk : tracksToNotify)
	{
		NotifyTrackFinished(trk.first, trk.second.c_str());
	}
	if (notifyNextStarted)
	{
		NotifyTrackStarted(nextStartedId, nextStartedName.c_str());
	}
	if (notifyState)
	{
		NotifyStateChanged(stateToNotify);
	}
}

size_t VoiceCore::GetClientQueueSize(uint32_t clientId) const
{
	EnterCriticalSection(&m_lock);
	size_t count = 0;
	for (const auto& t : m_queue)
	{
		if (t && (clientId == 0 || t->ownerId == clientId))
			count++;
	}
	LeaveCriticalSection(&m_lock);
	return count;
}

void VoiceCore::ClearClientQueue(uint32_t clientId)
{
	if (clientId == 0)
		return;

	std::vector<std::pair<uint32_t, std::string>> tracksToNotify;
	EnterCriticalSection(&m_lock);
	for (auto it = m_queue.begin(); it != m_queue.end(); )
	{
		if ((*it)->ownerId == clientId)
		{
			tracksToNotify.push_back({ (*it)->GetEventTrackId(), (*it)->name });
			it = m_queue.erase(it);
		}
		else
		{
			++it;
		}
	}
	for (auto it = m_pausedStack.begin(); it != m_pausedStack.end(); )
	{
		if ((*it)->ownerId == clientId)
		{
			tracksToNotify.push_back({ (*it)->GetEventTrackId(), (*it)->name });
			it = m_pausedStack.erase(it);
		}
		else
		{
			++it;
		}
	}
	LeaveCriticalSection(&m_lock);

	for (const auto& trk : tracksToNotify)
	{
		NotifyTrackFinished(trk.first, trk.second.c_str());
	}
}

uint32_t VoiceCore::GetTrackOwner(uint32_t trackId) const
{
	if (trackId == 0)
		return 0;

	EnterCriticalSection(&m_lock);
	if (m_pCurrentTrack && m_pCurrentTrack->trackId == trackId)
	{
		uint32_t owner = m_pCurrentTrack->ownerId;
		LeaveCriticalSection(&m_lock);
		return owner;
	}
	for (const auto& t : m_queue)
	{
		if (t && t->trackId == trackId)
		{
			uint32_t owner = t->ownerId;
			LeaveCriticalSection(&m_lock);
			return owner;
		}
	}
	for (const auto& t : m_pausedStack)
	{
		if (t && t->trackId == trackId)
		{
			uint32_t owner = t->ownerId;
			LeaveCriticalSection(&m_lock);
			return owner;
		}
	}
	LeaveCriticalSection(&m_lock);

	// Fallback to high byte for core-generated namespaced track IDs
	return (trackId >> 24);
}

VoicePlaybackState VoiceCore::GetState() const
{
	EnterCriticalSection(&m_lock);
	VoicePlaybackState st = m_state;
	LeaveCriticalSection(&m_lock);
	return st;
}

bool VoiceCore::IsTransmitting() const
{
	if (m_bPanic.load(std::memory_order_relaxed))
		return false;
	return m_bPluginTransmitting.load(std::memory_order_relaxed)
		&& !m_bDucked.load(std::memory_order_relaxed)
		&& !m_bPaused.load(std::memory_order_relaxed);
}

size_t VoiceCore::GetQueueSize() const
{
	EnterCriticalSection(&m_lock);
	size_t sz = m_queue.size() + (m_pCurrentTrack ? 1 : 0);
	LeaveCriticalSection(&m_lock);
	return sz;
}

uint64_t VoiceCore::GetCurrentSteamID() const
{
	return QuerySteamID();
}

void VoiceCore::RegisterListener(IVoiceEventListener* listener)
{
	if (!listener) return;
	EnterCriticalSection(&m_lock);
	if (std::find(m_listeners.begin(), m_listeners.end(), listener) == m_listeners.end())
	{
		m_listeners.push_back(listener);
	}
	LeaveCriticalSection(&m_lock);
}

void VoiceCore::UnregisterListener(IVoiceEventListener* listener)
{
	if (!listener) return;
	EnterCriticalSection(&m_lock);
	auto it = std::find(m_listeners.begin(), m_listeners.end(), listener);
	if (it != m_listeners.end())
	{
		m_listeners.erase(it);
	}
	LeaveCriticalSection(&m_lock);
}

void VoiceCore::SetUserTalking(bool talking)
{
	bool notifyState = false;
	VoicePlaybackState stateToNotify = VoicePlaybackState::IDLE;

	EnterCriticalSection(&m_lock);
	m_bUserTalking.store(talking, std::memory_order_relaxed);

	if (talking)
	{
		m_duckResumeQPC.store(0, std::memory_order_relaxed);

		if (m_state == VoicePlaybackState::PLAYING && m_pCurrentTrack && m_pCurrentTrack->params.allowDucking)
		{
			notifyState = SetStateLocked(VoicePlaybackState::DUCKED, stateToNotify);
			QueryPerformanceCounter(&m_duckStartTime);
		}
	}
	else
	{
		if (m_state == VoicePlaybackState::DUCKED)
		{
			// Invariant: Debounce hysteresis prevents micro-stuttering and rapid audio thrashing.
			// Push-to-talk release introduces key-bounce and natural speech pauses. Instead of resuming
			// immediately when microphone state transitions to false, we arm m_duckResumeQPC with a debounce
			// window (ve_duck_resume_ms, default 150ms). If speech resumes before debounce expiry, the timer
			// cancels without audible resume glitches.
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			uint64_t debounceMs = 150;
			if (g_ve_duck_resume_ms && g_ve_duck_resume_ms->value >= 0.0f)
				debounceMs = static_cast<uint64_t>(g_ve_duck_resume_ms->value);
			uint64_t debounceQPC = 0;
			if (m_perfFreq.QuadPart > 0)
				debounceQPC = static_cast<uint64_t>(now.QuadPart)
					+ debounceMs * static_cast<uint64_t>(m_perfFreq.QuadPart) / 1000ull;
			m_duckResumeQPC.store(debounceQPC, std::memory_order_relaxed);
		}
	}
	LeaveCriticalSection(&m_lock);

	if (notifyState)
	{
		NotifyStateChanged(stateToNotify);
	}
}

bool VoiceCore::SetStateLocked(VoicePlaybackState newState, VoicePlaybackState& outNotifiedState)
{
	if (m_state != newState)
	{
		m_state = newState;
		m_bDucked.store(newState == VoicePlaybackState::DUCKED, std::memory_order_relaxed);
		outNotifiedState = newState;
		return true;
	}
	return false;
}

void VoiceCore::Panic(const char* reason, bool recoverable)
{
	// A latched panic must never be downgraded to a recoverable one.
	if (!recoverable)
		m_bPanicRecoverable.store(false, std::memory_order_relaxed);
	else if (!m_bPanic.load(std::memory_order_relaxed))
		m_bPanicRecoverable.store(true, std::memory_order_relaxed);

	m_bPanic.store(true, std::memory_order_seq_cst);
	m_bResetWatchdog.store(true, std::memory_order_release);

	std::vector<std::pair<uint32_t, std::string>> tracksToNotify;
	bool notifyState = false;
	VoicePlaybackState stateToNotify = VoicePlaybackState::IDLE;

	EnterCriticalSection(&m_lock);
	m_bPaused.store(false, std::memory_order_relaxed);
	m_pauseStartTime.QuadPart = 0;

	if (m_pCurrentTrack)
	{
		tracksToNotify.push_back({ m_pCurrentTrack->GetEventTrackId(), m_pCurrentTrack->name });
		m_pCurrentTrack.reset();
	}
	for (const auto& t : m_pausedStack)
	{
		if (t) tracksToNotify.push_back({ t->GetEventTrackId(), t->name });
	}
	for (const auto& t : m_queue)
	{
		if (t) tracksToNotify.push_back({ t->GetEventTrackId(), t->name });
	}
	m_pausedStack.clear();
	m_queue.clear();
	m_stream.active = false;
	m_stream.pcmAccumulator.clear();
	m_stream.resamplePhase = 0.0;
	m_duckResumeQPC.store(0, std::memory_order_relaxed);
	m_bPluginTransmitting.store(false, std::memory_order_relaxed);
	m_cachedSteamId.store(0, std::memory_order_relaxed);
	notifyState = SetStateLocked(VoicePlaybackState::IDLE, stateToNotify);

	if (reason && reason[0])
		strncpy_s(m_panicReason, sizeof(m_panicReason), reason, _TRUNCATE);
	else
		m_panicReason[0] = '\0';

	LeaveCriticalSection(&m_lock);

	// Invoke listeners outside m_lock to guarantee zero reentrancy deadlock
	for (const auto& trk : tracksToNotify)
	{
		NotifyTrackFinished(trk.first, trk.second.c_str());
	}
	if (notifyState)
	{
		NotifyStateChanged(stateToNotify);
	}
}

void VoiceCore::Unpanic()
{
	EnterCriticalSection(&m_lock);
	m_panicReason[0] = '\0';
	LeaveCriticalSection(&m_lock);

	m_bResetWatchdog.store(true, std::memory_order_release);
	m_recoverHealthySince = {};
	m_bPanicRecoverable.store(false, std::memory_order_relaxed);
	m_bPanic.store(false, std::memory_order_seq_cst);
}

// Recoverable panics (map change, SteamID not ready) clear themselves once the engine
// has remained healthy for 1 continuous second. Latched panics never clear automatically.
bool VoiceCore::TryAutoRecover()
{
	if (!m_bPanicRecoverable.load(std::memory_order_relaxed))
		return false;

	const bool healthy = g_clientStatePtr && *g_clientStatePtr == 5
		&& g_voiceSendMaxSize && *g_voiceSendMaxSize >= 512 && *g_voiceSendMaxSize <= 0x20000
		&& QuerySteamID() != 0;

	LARGE_INTEGER now{};
	QueryPerformanceCounter(&now);

	if (!healthy)
	{
		m_recoverHealthySince = {};
		return false;
	}
	if (m_recoverHealthySince.QuadPart == 0)
	{
		m_recoverHealthySince = now;
		return false;
	}

	if (m_perfFreq.QuadPart <= 0)
		return false;

	const uint64_t ms = static_cast<uint64_t>(now.QuadPart - m_recoverHealthySince.QuadPart) * 1000ull
		/ static_cast<uint64_t>(m_perfFreq.QuadPart);
	if (ms < 1000)
		return false;

	Unpanic();
	return true;
}

void VoiceCore::StopAll()
{
	VE_DEBUG_LOG("[VoiceExtension] StopAll invoked (purging active track and queue).\n");
	m_bResetWatchdog.store(true, std::memory_order_release);

	std::vector<std::pair<uint32_t, std::string>> tracksToNotify;
	bool notifyState = false;
	VoicePlaybackState stateToNotify = VoicePlaybackState::IDLE;

	EnterCriticalSection(&m_lock);
	m_bPaused.store(false, std::memory_order_relaxed);
	m_pauseStartTime.QuadPart = 0;
	if (m_pCurrentTrack)
	{
		tracksToNotify.push_back({ m_pCurrentTrack->GetEventTrackId(), m_pCurrentTrack->name });
		m_pCurrentTrack.reset();
	}
	for (const auto& t : m_pausedStack)
	{
		if (t) tracksToNotify.push_back({ t->GetEventTrackId(), t->name });
	}
	for (const auto& t : m_queue)
	{
		if (t) tracksToNotify.push_back({ t->GetEventTrackId(), t->name });
	}
	m_pausedStack.clear();
	m_queue.clear();
	m_stream.active = false;
	m_stream.pcmAccumulator.clear();
	m_stream.resamplePhase = 0.0;
	m_bPluginTransmitting.store(false, std::memory_order_relaxed);
	m_cachedSteamId.store(0, std::memory_order_relaxed);
	m_hangtimeClientId = 0;
	m_hangtimeStartQPC.QuadPart = 0;
	notifyState = SetStateLocked(VoicePlaybackState::IDLE, stateToNotify);
	LeaveCriticalSection(&m_lock);

	for (const auto& trk : tracksToNotify)
	{
		NotifyTrackFinished(trk.first, trk.second.c_str());
	}
	if (notifyState)
	{
		NotifyStateChanged(stateToNotify);
	}
}

void VoiceCore::StopCurrent()
{
	uint32_t finishedId = 0;
	std::string finishedName;
	bool notifyFinished = false;

	uint32_t nextStartedId = 0;
	std::string nextStartedName;
	bool notifyNextStarted = false;

	bool notifyState = false;
	VoicePlaybackState stateToNotify = VoicePlaybackState::IDLE;

	EnterCriticalSection(&m_lock);
	if (m_pCurrentTrack)
	{
		notifyFinished = true;
		finishedId = m_pCurrentTrack->GetEventTrackId();
		finishedName = m_pCurrentTrack->name;
		m_pCurrentTrack.reset();
	}

	AdvanceTrackLocked(notifyNextStarted, nextStartedId, nextStartedName, notifyState, stateToNotify);
	LeaveCriticalSection(&m_lock);

	if (notifyFinished)
	{
		NotifyTrackFinished(finishedId, finishedName.c_str());
	}
	if (notifyNextStarted)
	{
		NotifyTrackStarted(nextStartedId, nextStartedName.c_str());
	}
	if (notifyState)
	{
		NotifyStateChanged(stateToNotify);
	}
}

void VoiceCore::ClearQueue()
{
	std::vector<std::pair<uint32_t, std::string>> tracksToNotify;
	EnterCriticalSection(&m_lock);
	for (const auto& t : m_queue)
	{
		if (t) tracksToNotify.push_back({ t->GetEventTrackId(), t->name });
	}
	for (const auto& t : m_pausedStack)
	{
		if (t) tracksToNotify.push_back({ t->GetEventTrackId(), t->name });
	}
	m_queue.clear();
	m_pausedStack.clear();
	LeaveCriticalSection(&m_lock);

	for (const auto& trk : tracksToNotify)
	{
		NotifyTrackFinished(trk.first, trk.second.c_str());
	}
}

void VoiceCore::Pause(bool pause)
{
	EnterCriticalSection(&m_lock);
	if (pause && !m_bPaused.load(std::memory_order_relaxed))
	{
		if (m_pCurrentTrack || !m_queue.empty())
		{
			m_bPaused.store(true, std::memory_order_relaxed);
			QueryPerformanceCounter(&m_pauseStartTime);
		}
	}
	else if (!pause && m_bPaused.load(std::memory_order_relaxed))
	{
		m_bPaused.store(false, std::memory_order_relaxed);
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if (m_pauseStartTime.QuadPart > 0 && now.QuadPart >= m_pauseStartTime.QuadPart)
		{
			// Invariant: Advance m_playbackT0 by the pause duration to maintain continuous cadence upon unpause.
			uint64_t pauseDuration = static_cast<uint64_t>(now.QuadPart - m_pauseStartTime.QuadPart);
			m_playbackT0.QuadPart += pauseDuration;

			// If ducking was also active during this pause, shift m_duckStartTime forward by pauseDuration
			// so the pause duration is not double-compensated when ducking ends.
			if (m_duckStartTime.QuadPart > 0)
			{
				m_duckStartTime.QuadPart += pauseDuration;
			}
		}
	}
	LeaveCriticalSection(&m_lock);
}

bool VoiceCore::StampAndInjectChunk(const OpusChunk* frames, size_t frameCount,
	uint32_t& sentBytes, uint64_t steamId)
{
	if (!g_voiceSendData || !*g_voiceSendData || !g_voiceSendCursor || !g_voiceSendMaxSize)
		return false;
	if (!frames || frameCount == 0 || frameCount > kMaxFramesPerVoicePacket)
		return false;
	if (steamId == 0)
		return false;

	// The server rejects voice blocks above 4096 bytes (SV_ParseVoiceData).
	uint8_t pkt[4096];
	size_t pos = 0;

	for (int i = 0; i < 8; ++i)
		pkt[pos++] = static_cast<uint8_t>((steamId >> (i * 8)) & 0xFF);

	pkt[pos++] = 11; // Payload type: Steam Voice data (clc_voicedata sub-protocol)

	pkt[pos++] = static_cast<uint8_t>(kTargetSampleRate & 0xFF);
	pkt[pos++] = static_cast<uint8_t>((kTargetSampleRate >> 8) & 0xFF);

	pkt[pos++] = 6;

	size_t payloadLenOffset = pos;
	pkt[pos++] = 0;
	pkt[pos++] = 0;

	size_t payloadStart = pos;
	uint16_t seqNext = m_globalSequence.load(std::memory_order_relaxed);
	size_t encodedFrames = 0;

	for (size_t i = 0; i < frameCount; ++i)
	{
		const auto& frame = frames[i].frame;
		if (frame.empty())
			continue;
		if (frame.size() > UINT16_MAX || pos + 4 + frame.size() + 4 > sizeof(pkt))
			return false;

		uint16_t flen = static_cast<uint16_t>(frame.size());
		uint16_t fseq = seqNext++;
		pkt[pos++] = static_cast<uint8_t>(flen & 0xFF);
		pkt[pos++] = static_cast<uint8_t>((flen >> 8) & 0xFF);
		pkt[pos++] = static_cast<uint8_t>(fseq & 0xFF);
		pkt[pos++] = static_cast<uint8_t>((fseq >> 8) & 0xFF);
		memcpy(pkt + pos, frame.data(), flen);
		pos += flen;
		++encodedFrames;
	}

	if (encodedFrames == 0)
		return false;

	const size_t payloadLen = pos - payloadStart;
	if (payloadLen > UINT16_MAX)
		return false;
	pkt[payloadLenOffset] = static_cast<uint8_t>(payloadLen & 0xFF);
	pkt[payloadLenOffset + 1] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);

	uint32_t crc = CalculateCrc32(pkt, pos);
	pkt[pos++] = static_cast<uint8_t>(crc & 0xFF);
	pkt[pos++] = static_cast<uint8_t>((crc >> 8) & 0xFF);
	pkt[pos++] = static_cast<uint8_t>((crc >> 16) & 0xFF);
	pkt[pos++] = static_cast<uint8_t>((crc >> 24) & 0xFF);

	const uint32_t need = static_cast<uint32_t>(pos) + 3;
	const uint32_t cursize = *g_voiceSendCursor;
	const uint32_t maxsize = *g_voiceSendMaxSize;
	const uint32_t safeLimit = (maxsize < kMaxSafeVoiceBuffer) ? maxsize : kMaxSafeVoiceBuffer;

	if (maxsize == 0 || cursize > maxsize)
	{
		return false;
	}
	if (need > kMaxInjectBytesPerFrame)
		return false;
	if (static_cast<uint64_t>(cursize) + need >= safeLimit)
	{
		return false;
	}
	if (sentBytes && sentBytes + need > kMaxInjectBytesPerFrame)
	{
		return false;
	}

	uint8_t* dst = *g_voiceSendData + cursize;
	dst[0] = kClcVoiceData;
	dst[1] = static_cast<uint8_t>(pos & 0xFF);
	dst[2] = static_cast<uint8_t>((pos >> 8) & 0xFF);
	memcpy(dst + 3, pkt, pos);
	*g_voiceSendCursor = cursize + need;
	m_globalSequence.store(seqNext, std::memory_order_relaxed);

	sentBytes += need;
	m_stats.injectedFrames += static_cast<uint64_t>(encodedFrames);
	m_stats.injectedPackets++;
	return true;
}

void VoiceCore::NotifyTrackStarted(uint32_t trackId, const char* name)
{
	std::vector<IVoiceEventListener*> copy;
	EnterCriticalSection(&m_lock);
	copy = m_listeners;
	LeaveCriticalSection(&m_lock);

	for (auto* l : copy)
	{
		if (l) l->OnTrackStarted(trackId, name);
	}
}

void VoiceCore::NotifyTrackFinished(uint32_t trackId, const char* name)
{
	std::vector<IVoiceEventListener*> copy;
	EnterCriticalSection(&m_lock);
	copy = m_listeners;
	LeaveCriticalSection(&m_lock);

	for (auto* l : copy)
	{
		if (l) l->OnTrackFinished(trackId, name);
	}
}

void VoiceCore::NotifyStateChanged(VoicePlaybackState newState)
{
	std::vector<IVoiceEventListener*> copy;
	EnterCriticalSection(&m_lock);
	copy = m_listeners;
	LeaveCriticalSection(&m_lock);

	for (auto* l : copy)
	{
		if (l) l->OnStateChanged(newState);
	}
}

void VoiceCore::OnDecompressVoice(uint32_t writtenBytes)
{
	m_stats.rxPackets++;
	m_stats.rxBytes += writtenBytes;

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	if (m_rxLastCounter.QuadPart && m_perfFreq.QuadPart && now.QuadPart >= m_rxLastCounter.QuadPart)
	{
		uint32_t gapUs = static_cast<uint32_t>((now.QuadPart - m_rxLastCounter.QuadPart) * 1000000ull / m_perfFreq.QuadPart);
		if (gapUs > m_stats.maxRxGapUs)
			m_stats.maxRxGapUs = gapUs;
		if (gapUs > 75000)
			m_stats.rxJitterCount++;
	}
	m_rxLastCounter = now;
}

bool VoiceCore::InjectDueChunks()
{
	// Process lock-free watchdog reset requested by StopAll/Panic/Unpanic
	if (m_bResetWatchdog.exchange(false, std::memory_order_acquire))
	{
		m_watchdog = {};
		m_injectedLastFrame = false;
	}

	// (a) Panic check with auto-recovery
	if (m_bPanic.load(std::memory_order_relaxed))
	{
		if (!TryAutoRecover())
			return false;
	}

	if (m_bPaused.load(std::memory_order_relaxed))
		return false;

	// (b) Fail closed: without the state pointer we cannot tell a map change from a live session
	if (!g_clientStatePtr)
		return false;

	if (*g_clientStatePtr != 5)
	{
		m_cachedSteamId.store(0, std::memory_order_relaxed);
		m_stats.droppedStateNot5++;

		// Disconnected, loading, or changing map: stop immediately so audio tails do not spill
		// into the next map, and prevent triggering a false panic watchdog during level load.
		if (m_bPluginTransmitting.load(std::memory_order_relaxed) || GetQueueSize() > 0)
		{
			VE_DEBUG_LOG("[VoiceExtension] Client state not 5 (*state=%d), stopping all playback.\n", *g_clientStatePtr);
			StopAll();
		}
		m_watchdog.stateNot5StartTime = {};
		return false;
	}
	m_watchdog.stateNot5StartTime = {};

	if (!g_voiceSendData || !*g_voiceSendData || !g_voiceSendCursor || !g_voiceSendMaxSize)
		return false;

	// Watchdog: sizebuf layout and bounds check (unrecoverable engine layout mismatch)
	if (*g_voiceSendMaxSize < 512 || *g_voiceSendMaxSize > 0x20000 || *g_voiceSendCursor > *g_voiceSendMaxSize)
	{
		if (m_bPluginTransmitting.load(std::memory_order_relaxed))
		{
			Panic("Watchdog: sizebuf layout mismatch or cursor overflow", false);
		}
		return false;
	}

	if (!m_bPluginTransmitting.load(std::memory_order_relaxed))
	{
		m_watchdog.unflushedStartTime = {};
		m_watchdog.steamIdZeroStartTime = {};
		return false;
	}

	// Watchdog: unflushed sizebuf stall detection (time-based)
	if (m_injectedLastFrame && *g_voiceSendCursor > 0)
	{
		m_stats.unflushedFrames++;
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		if (m_watchdog.unflushedStartTime.QuadPart == 0)
		{
			m_watchdog.unflushedStartTime = now;
		}
		else if (m_perfFreq.QuadPart > 0)
		{
			uint64_t elapsedMs = static_cast<uint64_t>(now.QuadPart - m_watchdog.unflushedStartTime.QuadPart) * 1000ull / static_cast<uint64_t>(m_perfFreq.QuadPart);
			if (elapsedMs >= 1000)
			{
				Panic("Watchdog: engine sizebuf stalled (unflushed for >1000ms)", true);
				m_watchdog.unflushedStartTime = {};
				return false;
			}
		}
	}
	else if (*g_voiceSendCursor == 0)
	{
		m_watchdog.unflushedStartTime = {};
	}
	m_injectedLastFrame = false;

	// Watchdog: SteamID check while transmitting.
	// Done before acquiring m_lock so that if Steam drops or reconnects slowly,
	// Panic(..., true) executes completely outside m_lock without holding locks during listener notifications.
	uint64_t steamId = QuerySteamID();
	if (steamId == 0)
	{
		m_stats.droppedSteamIdZero++;
		LARGE_INTEGER nowWatchdog{};
		QueryPerformanceCounter(&nowWatchdog);
		if (m_watchdog.steamIdZeroStartTime.QuadPart == 0)
		{
			m_watchdog.steamIdZeroStartTime = nowWatchdog;
		}
		else if (m_perfFreq.QuadPart > 0)
		{
			uint64_t elapsedMs = static_cast<uint64_t>(nowWatchdog.QuadPart - m_watchdog.steamIdZeroStartTime.QuadPart) * 1000ull / static_cast<uint64_t>(m_perfFreq.QuadPart);
			if (elapsedMs >= 1000)
			{
				Panic("Watchdog: SteamID is 0 for >1000ms during playback", true);
				m_watchdog.steamIdZeroStartTime = {};
			}
		}
		return false;
	}
	m_watchdog.steamIdZeroStartTime = {};

	uint32_t notifyFinishedId = 0;
	std::string notifyFinishedName;
	bool haveFinishedTrack = false;

	uint32_t notifyStartedId = 0;
	std::string notifyStartedName;
	bool haveStartedTrack = false;

	bool notifyState = false;
	VoicePlaybackState stateToNotify = VoicePlaybackState::IDLE;

	EnterCriticalSection(&m_lock);

	if (m_state == VoicePlaybackState::DUCKED)
	{
		if (m_bUserTalking.load(std::memory_order_relaxed))
		{
			m_duckResumeQPC.store(0, std::memory_order_relaxed);
			LeaveCriticalSection(&m_lock);
			return false;
		}

		const uint64_t resumeQPC = m_duckResumeQPC.load(std::memory_order_relaxed);
		if (resumeQPC == 0)
		{
			LeaveCriticalSection(&m_lock);
			return false;
		}
		LARGE_INTEGER nowDebounce{};
		QueryPerformanceCounter(&nowDebounce);
		if (static_cast<uint64_t>(nowDebounce.QuadPart) < resumeQPC)
		{
			LeaveCriticalSection(&m_lock);
			return false;
		}

		if (m_duckStartTime.QuadPart > 0 && nowDebounce.QuadPart >= m_duckStartTime.QuadPart)
		{
			// Invariant: m_playbackT0 represents the wall-clock anchor corresponding to track timestamp 0.
			// When playback is ducked, wall time elapses while chunk delivery is suspended.
			// Advance m_playbackT0 by the ducked duration to prevent all suspended frames from bursting immediately.
			m_playbackT0.QuadPart += nowDebounce.QuadPart - m_duckStartTime.QuadPart;
			m_duckStartTime = {};
		}
		m_duckResumeQPC.store(0, std::memory_order_relaxed);
		notifyState = SetStateLocked(VoicePlaybackState::PLAYING, stateToNotify);
	}

	if (!m_pCurrentTrack)
	{
		// Check if we're in hangtime
		if (m_hangtimeClientId != 0 && m_hangtimeStartQPC.QuadPart != 0 && m_perfFreq.QuadPart > 0)
		{
			LARGE_INTEGER nowHt{};
			QueryPerformanceCounter(&nowHt);
			const uint64_t elapsedHtMs = static_cast<uint64_t>(nowHt.QuadPart - m_hangtimeStartQPC.QuadPart)
				* 1000ull / static_cast<uint64_t>(m_perfFreq.QuadPart);

			auto pit = m_clientParams.find(m_hangtimeClientId);
			const uint32_t hangtimeMs = (pit != m_clientParams.end()) ? pit->second.hangtimeMs : 0;

			if (elapsedHtMs < hangtimeMs)
			{
				// Still within hangtime window: hold mic, inject nothing, let engine flush naturally
				LeaveCriticalSection(&m_lock);
				return true;
			}
		}
		// Hangtime expired or not set: go IDLE
		if (m_hangtimeClientId != 0)
		{
			VE_DEBUG_LOG("[VoiceExtension] Hangtime expired (clientId=%u): going IDLE\n", m_hangtimeClientId);
		}
		m_hangtimeClientId = 0;
		m_hangtimeStartQPC.QuadPart = 0;
		m_bPluginTransmitting.store(false, std::memory_order_relaxed);
		notifyState = SetStateLocked(VoicePlaybackState::IDLE, stateToNotify);
		LeaveCriticalSection(&m_lock);
		if (notifyState)
		{
			NotifyStateChanged(stateToNotify);
		}
		return false;
	}

	LARGE_INTEGER now{};
	QueryPerformanceCounter(&now);

	uint64_t elapsedUs = 0;
	if (m_perfFreq.QuadPart > 0 && now.QuadPart >= m_playbackT0.QuadPart)
	{
		elapsedUs = static_cast<uint64_t>(now.QuadPart - m_playbackT0.QuadPart) * 1000000ull / static_cast<uint64_t>(m_perfFreq.QuadPart);
	}

	// Invariant: Hitch and Alt-Tab clock compensation for audio files.
	// If the game engine freezes, stalls, or hitches for >200ms, elapsedUs jumps far ahead of chunk dueUs.
	// Without compensation, the engine would dump all overdue chunks in a rapid high-speed burst.
	// Shifting m_playbackT0 by lateUs cleanly pauses playback during the freeze and resumes at normal 1.0x cadence.
	if (!m_pCurrentTrack->isStream && m_pCurrentTrack->currentChunk < m_pCurrentTrack->chunks.size())
	{
		const uint64_t due = m_pCurrentTrack->chunks[m_pCurrentTrack->currentChunk].dueUs;
		constexpr uint64_t kMaxLateUs = 200000;
		if (elapsedUs > due + kMaxLateUs)
		{
			const uint64_t lateUs = elapsedUs - due;
			m_playbackT0.QuadPart += static_cast<LONGLONG>(lateUs * m_perfFreq.QuadPart / 1000000ull);
			elapsedUs = due;
		}
	}

	// Jitter Buffer Lead Time (headroom):
	// Voice_Send runs at the engine's command cadence (~50ms in the observed client),
	// while Opus frames are 20ms. Without lookahead a strict wall-clock comparison
	// (dueUs > elapsedUs) starves the GoldSrc receiver: only one 20ms frame would be
	// delivered per 50ms tick. An 80ms cushion (four frames) lets every call carry the
	// 2..3 frames captured since the previous call, matching the engine's own blocks.
	constexpr uint64_t kJitterLeadTimeUs = 80000;
	const uint64_t targetUs = elapsedUs + kJitterLeadTimeUs;

	uint32_t sentBytes = 0;

	// The engine sends one voice block per Voice_Send call, carrying every frame
	// Steams GetVoice produced since the previous call (1..8 frames, avg 2.5).
	// Reproduce that shape: pack all frames that are due into a single block instead
	// of emitting one block per 40ms of audio (which can put two blocks in one tick).
	size_t dueCount = 0;
	{
		const auto& chunks = m_pCurrentTrack->chunks;
		while (m_pCurrentTrack->currentChunk + dueCount < chunks.size() &&
			chunks[m_pCurrentTrack->currentChunk + dueCount].dueUs <= targetUs &&
			dueCount < kMaxFramesPerVoicePacket)
		{
			++dueCount;
		}
	}

	if (dueCount > 0)
	{
		const OpusChunk* group = m_pCurrentTrack->chunks.data() + m_pCurrentTrack->currentChunk;
		while (dueCount > 0)
		{
			// A hitch can leave more frames due than fit into one block; deliver what
			// fits and leave the rest for the next call instead of splitting the tick.
			if (StampAndInjectChunk(group, dueCount, sentBytes, steamId))
			{
				m_pCurrentTrack->currentChunk += dueCount;
				m_injectedLastFrame = true;
				break;
			}
			if (dueCount == 1)
				break;
			--dueCount;
		}
	}

	if (dueCount > 0 && sentBytes == 0)
		m_stats.droppedBufferFull++;

	if (!m_pCurrentTrack)
	{
		LeaveCriticalSection(&m_lock);
		return false;
	}

	// (c) All chunks delivered branch
	if (m_pCurrentTrack->currentChunk >= m_pCurrentTrack->chunks.size())
	{
		if (m_pCurrentTrack->isStream && m_stream.active)
		{
			constexpr uint64_t kStreamIdleTimeoutMs = 2000;
			uint64_t idleMs = 0;
			if (m_streamLastPushQPC.QuadPart > 0 && now.QuadPart >= m_streamLastPushQPC.QuadPart && m_perfFreq.QuadPart > 0)
			{
				idleMs = static_cast<uint64_t>(now.QuadPart - m_streamLastPushQPC.QuadPart) * 1000ull
					/ static_cast<uint64_t>(m_perfFreq.QuadPart);
			}

			if (idleMs < kStreamIdleTimeoutMs)
			{
				if (elapsedUs > m_stream.nextDueUs + 80000)
				{
					// Invariant: Live PCM streaming clock drift correction.
					// If producer delivery lags behind client rendering by >80ms, re-anchor m_playbackT0
					// so that audio does not build up artificial pipeline latency.
					m_playbackT0.QuadPart = now.QuadPart - static_cast<LONGLONG>(m_stream.nextDueUs * m_perfFreq.QuadPart / 1000000ull);
				}
				LeaveCriticalSection(&m_lock);
				if (notifyState)
				{
					NotifyStateChanged(stateToNotify);
				}
				return (sentBytes > 0);
			}

			// Producer went silent (crashed or never closed stream): close stream and fall through to track finish
			m_stream.active = false;
			m_stream.pcmAccumulator.clear();
		}

		if (!m_pCurrentTrack->isStream)
		{
			const uint64_t trackTotalUs = m_pCurrentTrack->chunks.empty() ? 0 :
				(m_pCurrentTrack->chunks.back().dueUs + kFrameDurationUs);

			if (elapsedUs < trackTotalUs)
			{
				// All chunks have been staged into the network pipeline with jitter lead time,
				// but the audio waveform is still playing on listeners' speakers.
				LeaveCriticalSection(&m_lock);
				if (notifyState)
				{
					NotifyStateChanged(stateToNotify);
				}
				return true;
			}
		}

		if (m_pCurrentTrack->params.loop && !m_pCurrentTrack->isStream)
		{
			m_pCurrentTrack->currentChunk = 0;
			m_playbackT0 = now;
		}
		else
		{
			haveFinishedTrack = true;
			notifyFinishedId = m_pCurrentTrack->GetEventTrackId();
			notifyFinishedName = m_pCurrentTrack->name;
			const uint32_t finishedOwnerId = m_pCurrentTrack->ownerId;
			VE_DEBUG_LOG("[VoiceExtension] Track finished: id=0x%08X (eventTrackId=%u, name='%s')\n",
				m_pCurrentTrack->trackId, notifyFinishedId, notifyFinishedName.c_str());
			m_pCurrentTrack.reset();

			AdvanceTrackLocked(haveStartedTrack, notifyStartedId, notifyStartedName, notifyState, stateToNotify);

			// If queue is now empty and the finished track's client has hangtime configured,
			// activate hangtime: keep m_bPluginTransmitting=true for the client's declared duration.
			if (!m_pCurrentTrack && m_queue.empty() && m_pausedStack.empty() && finishedOwnerId != 0)
			{
				auto pit = m_clientParams.find(finishedOwnerId);
				if (pit != m_clientParams.end() && pit->second.hangtimeMs > 0)
				{
					// Override the IDLE that AdvanceTrackLocked just set
					m_bPluginTransmitting.store(true, std::memory_order_relaxed);
					notifyState = SetStateLocked(VoicePlaybackState::PLAYING, stateToNotify);
					m_hangtimeClientId = finishedOwnerId;
					QueryPerformanceCounter(&m_hangtimeStartQPC);
					haveStartedTrack = false; // suppress spurious OnTrackStarted
					VE_DEBUG_LOG("[VoiceExtension] Hangtime active: clientId=%u, %u ms\n",
						finishedOwnerId, pit->second.hangtimeMs);
				}
			}
		}
	}

	LeaveCriticalSection(&m_lock);

	// Safe callback delivery outside m_lock
	if (haveFinishedTrack)
	{
		NotifyTrackFinished(notifyFinishedId, notifyFinishedName.c_str());
	}
	if (haveStartedTrack)
	{
		NotifyTrackStarted(notifyStartedId, notifyStartedName.c_str());
	}
	if (notifyState)
	{
		NotifyStateChanged(stateToNotify);
	}

	return (sentBytes > 0);
}

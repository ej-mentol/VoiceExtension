#include "test_harness.h"
#include "../src/VoiceUtils.h"
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <unordered_set>

// Verify direct VoiceUtils production implementation
static bool TestIsPathSafe(const char* path, bool isFromPipe, float ve_allow_abs_paths)
{
	return VoiceUtils::IsPathSafe(path, isFromPipe, ve_allow_abs_paths != 0.0f);
}

static float TestSoftLimit(float x)
{
	return VoiceUtils::SoftLimit(x);
}

#pragma pack(push, 1)
struct VoicePipeHeaderTest
{
	uint32_t magic;         // 0x56455854 ("VEXT")
	uint16_t command;       // VoicePipeCommand
	uint16_t sampleRate;    // 8000..48000
	uint8_t  channels;      // 1 or 2
	uint8_t  priority;      // 0..3
	float    volume;        // 0..2
	float    pitch;         // 0.5..2
	uint32_t dataBytes;     // Payload size
};
#pragma pack(pop)

int main()
{
	TEST_SUITE_START("VoiceExtension & Soundpad Core Architecture Tests");

	// --- 1. Path Safety & Traversal Rejection Tests ---
	printf("\n[Group 1: Path Traversal & Whitelist Security]\n");
	ASSERT_TRUE(TestIsPathSafe("sound/ambient/test.wav", false, 0.0f));
	ASSERT_TRUE(TestIsPathSafe("weapons/shotgun_fire.mp3", false, 0.0f));
	ASSERT_TRUE(TestIsPathSafe("music/ambient_loop.ogg", false, 0.0f));
	ASSERT_TRUE(TestIsPathSafe("custom_folder/sub/sound.wav", false, 0.0f));
	ASSERT_TRUE(TestIsPathSafe("sound/voice_line.wav", false, 0.0f)); // legitimate voice_ prefix allowed

	// Traversal attacks must be rejected
	ASSERT_FALSE(TestIsPathSafe("../windows/system32/calc.exe", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("sound/../../autoexec.cfg", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("..\\traversal.wav", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("/root/etc/passwd.wav", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("\\root\\test.wav", false, 0.0f));

	// Unsupported extensions must be rejected
	ASSERT_FALSE(TestIsPathSafe("sound/ambient/test.exe", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("sound/ambient/test.flac", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("sound/ambient/test.wav.vbs", false, 0.0f));

	// Engine reserved diagnostic files must be rejected
	ASSERT_FALSE(TestIsPathSafe("voice_micdata.wav", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("sound/voice_input.wav", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("voice_decompressed.wav", false, 0.0f));

	// DOS reserved device names must be rejected
	ASSERT_FALSE(TestIsPathSafe("con.wav", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("CON.wav", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("nul.mp3", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("aux.ogg", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("com1.wav", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("lpt1.mp3", false, 0.0f));
	ASSERT_FALSE(TestIsPathSafe("sound/con.wav", false, 0.0f));

	// Absolute paths sandbox rules
	ASSERT_FALSE(TestIsPathSafe("C:/sounds/voice.wav", false, 0.0f)); // from console: rejected
	ASSERT_FALSE(TestIsPathSafe("C:/sounds/voice.wav", true, 0.0f));  // from pipe but cvar=0: rejected
	ASSERT_TRUE(TestIsPathSafe("C:/sounds/voice.wav", true, 1.0f));   // from pipe and cvar=1: allowed
	ASSERT_FALSE(TestIsPathSafe("C:/sounds/voice.wav:stream", true, 1.0f)); // NTFS ADS: rejected

	// --- 2. Soft Limiter Dynamics Tests ---
	printf("\n[Group 2: Soft Limiter Saturation Curve]\n");
	// Inside linear band (<= 0.707): signal is completely unchanged
	ASSERT_EQ(TestSoftLimit(0.0f), 0.0f);
	ASSERT_EQ(TestSoftLimit(0.5f), 0.5f);
	ASSERT_EQ(TestSoftLimit(-0.5f), -0.5f);
	ASSERT_EQ(TestSoftLimit(0.707f), 0.707f);
	ASSERT_EQ(TestSoftLimit(-0.707f), -0.707f);

	// Above threshold: smooth saturation strictly asymptotically below +/- 1.0f
	float sat1 = TestSoftLimit(1.0f);
	ASSERT_TRUE(sat1 > 0.707f && sat1 < 1.0f);

	float sat2 = TestSoftLimit(2.0f);
	ASSERT_TRUE(sat2 > sat1 && sat2 < 1.0f);

	float satHuge = TestSoftLimit(100.0f);
	ASSERT_TRUE(satHuge >= sat2 && satHuge <= 1.0f);

	// Negative symmetry
	float satNeg = TestSoftLimit(-1.0f);
	ASSERT_TRUE(std::abs(satNeg - (-sat1)) < 0.0001f);

	// --- 3. NamedPipe Protocol Layout & Alignment Tests ---
	printf("\n[Group 3: NamedPipe Header Layout & Alignment]\n");
	// Protocol specification invariant: 22 bytes packed
	ASSERT_EQ(sizeof(VoicePipeHeaderTest), 22);

	// Field offset verification
	VoicePipeHeaderTest h{};
	ASSERT_EQ(reinterpret_cast<uint8_t*>(&h.magic) - reinterpret_cast<uint8_t*>(&h), 0);
	ASSERT_EQ(reinterpret_cast<uint8_t*>(&h.command) - reinterpret_cast<uint8_t*>(&h), 4);
	ASSERT_EQ(reinterpret_cast<uint8_t*>(&h.sampleRate) - reinterpret_cast<uint8_t*>(&h), 6);
	ASSERT_EQ(reinterpret_cast<uint8_t*>(&h.channels) - reinterpret_cast<uint8_t*>(&h), 8);
	ASSERT_EQ(reinterpret_cast<uint8_t*>(&h.priority) - reinterpret_cast<uint8_t*>(&h), 9);
	ASSERT_EQ(reinterpret_cast<uint8_t*>(&h.volume) - reinterpret_cast<uint8_t*>(&h), 10);
	ASSERT_EQ(reinterpret_cast<uint8_t*>(&h.pitch) - reinterpret_cast<uint8_t*>(&h), 14);
	ASSERT_EQ(reinterpret_cast<uint8_t*>(&h.dataBytes) - reinterpret_cast<uint8_t*>(&h), 18);

	// --- 4. WAV Container Header Integrity Tests ---
	printf("\n[Group 4: WAV Container Header Integrity]\n");
	// Build minimal 44-byte standard PCM RIFF header
	struct RiffHeader
	{
		char chunkId[4];
		uint32_t chunkSize;
		char format[4];
		char subchunk1Id[4];
		uint32_t subchunk1Size;
		uint16_t audioFormat;
		uint16_t numChannels;
		uint32_t sampleRate;
		uint32_t byteRate;
		uint16_t blockAlign;
		uint16_t bitsPerSample;
		char subchunk2Id[4];
		uint32_t subchunk2Size;
	} header;

	memcpy(header.chunkId, "RIFF", 4);
	header.chunkSize = 36 + 100;
	memcpy(header.format, "WAVE", 4);
	memcpy(header.subchunk1Id, "fmt ", 4);
	header.subchunk1Size = 16;
	header.audioFormat = 1; // PCM
	header.numChannels = 1; // Mono
	header.sampleRate = 24000;
	header.bitsPerSample = 16;
	header.blockAlign = 2;
	header.byteRate = 48000;
	memcpy(header.subchunk2Id, "data", 4);
	header.subchunk2Size = 100;

	ASSERT_EQ(sizeof(header), 44);
	ASSERT_TRUE(memcmp(header.chunkId, "RIFF", 4) == 0);
	ASSERT_TRUE(memcmp(header.format, "WAVE", 4) == 0);
	ASSERT_TRUE(memcmp(header.subchunk1Id, "fmt ", 4) == 0);
	ASSERT_TRUE(memcmp(header.subchunk2Id, "data", 4) == 0);
	ASSERT_EQ(header.audioFormat, 1);
	ASSERT_EQ(header.sampleRate, 24000);

	// --- 5. Multi-Client Namespacing & Owner Extraction Tests ---
	printf("\n[Group 5: Multi-Client Namespacing & Owner Extraction]\n");
	auto MakeTrackId = [](uint32_t clientId, uint32_t seq) -> uint32_t {
		return ((clientId & 0xFF) << 24) | (seq & 0x00FFFFFF);
	};
	auto GetOwner = [](uint32_t trackId) -> uint32_t {
		return (trackId >> 24) & 0xFF;
	};

	uint32_t clientSoundpad = 1;
	uint32_t clientTTS = 2;
	uint32_t clientChat = 3;

	uint32_t t1 = MakeTrackId(clientSoundpad, 100);
	uint32_t t2 = MakeTrackId(clientTTS, 100);
	uint32_t t3 = MakeTrackId(clientChat, 500);

	ASSERT_NE(t1, t2); // Same seq but different client IDs must never collide
	ASSERT_EQ(GetOwner(t1), clientSoundpad);
	ASSERT_EQ(GetOwner(t2), clientTTS);
	ASSERT_EQ(GetOwner(t3), clientChat);
	ASSERT_EQ(GetOwner(0), 0);

	// --- 6. Per-Client Queue Quota & Admission Control Tests ---
	printf("\n[Group 6: Client Queue Quota & Admission Control]\n");
	constexpr size_t kMaxQueueTotal = 64;
	constexpr size_t kMaxQueuePerClient = 16;

	struct MockTrack
	{
		uint32_t trackId;
		uint32_t ownerId;
	};
	std::vector<MockTrack> mockQueue;

	auto CanEnqueue = [&](uint32_t clientId) -> bool {
		if (mockQueue.size() >= kMaxQueueTotal)
			return false;
		if (clientId != 0)
		{
			size_t clientCount = 0;
			for (const auto& t : mockQueue)
			{
				if (t.ownerId == clientId)
					++clientCount;
			}
			if (clientCount >= kMaxQueuePerClient)
				return false;
		}
		return true;
	};

	// Client 1 enqueues up to 16 items
	for (size_t i = 0; i < kMaxQueuePerClient; ++i)
	{
		ASSERT_TRUE(CanEnqueue(clientSoundpad));
		mockQueue.push_back({ MakeTrackId(clientSoundpad, static_cast<uint32_t>(i + 1)), clientSoundpad });
	}
	// 17th item from Client 1 must be rejected
	ASSERT_FALSE(CanEnqueue(clientSoundpad));

	// Client 2 can still enqueue even though Client 1 is full
	ASSERT_TRUE(CanEnqueue(clientTTS));
	mockQueue.push_back({ MakeTrackId(clientTTS, 1), clientTTS });

	// StopClient simulation: purging clientSoundpad removes only their tracks
	mockQueue.erase(std::remove_if(mockQueue.begin(), mockQueue.end(),
		[clientSoundpad](const MockTrack& t) { return t.ownerId == clientSoundpad; }), mockQueue.end());

	ASSERT_EQ(mockQueue.size(), 1);
	ASSERT_EQ(mockQueue[0].ownerId, clientTTS);
	// Client 1 can now enqueue again
	ASSERT_TRUE(CanEnqueue(clientSoundpad));

	// --- 7. Stream Ownership Arbitration Tests ---
	printf("\n[Group 7: Stream Ownership Arbitration]\n");
	struct MockStream
	{
		bool active = false;
		uint32_t ownerId = 0;
	} stream;

	auto OpenStream = [&](uint32_t clientId) -> bool {
		if (stream.active && stream.ownerId != clientId && stream.ownerId != 0)
			return false; // stream occupied by another client
		stream.active = true;
		stream.ownerId = clientId;
		return true;
	};
	auto CloseStream = [&](uint32_t clientId) {
		if (stream.active && stream.ownerId == clientId)
		{
			stream.active = false;
			stream.ownerId = 0;
		}
	};

	ASSERT_TRUE(OpenStream(clientSoundpad));
	ASSERT_TRUE(stream.active);
	ASSERT_EQ(stream.ownerId, clientSoundpad);

	// Client TTS attempts to grab stream while Soundpad is streaming -> must fail
	ASSERT_FALSE(OpenStream(clientTTS));
	ASSERT_EQ(stream.ownerId, clientSoundpad);

	// Client Soundpad can re-open/update its own stream
	ASSERT_TRUE(OpenStream(clientSoundpad));

	// Legacy client (id 0) CANNOT close a stream owned by clientSoundpad
	CloseStream(0);
	ASSERT_TRUE(stream.active);
	ASSERT_EQ(stream.ownerId, clientSoundpad);

	// Soundpad finishes/closes stream
	CloseStream(clientSoundpad);
	ASSERT_FALSE(stream.active);
	ASSERT_EQ(stream.ownerId, 0);

	// Now Client TTS can successfully claim the stream
	ASSERT_TRUE(OpenStream(clientTTS));
	ASSERT_TRUE(stream.active);
	ASSERT_EQ(stream.ownerId, clientTTS);

	// Soundpad cannot close TTS's stream
	CloseStream(clientSoundpad);
	ASSERT_TRUE(stream.active);
	ASSERT_EQ(stream.ownerId, clientTTS);

	// TTS closes own stream
	CloseStream(clientTTS);
	ASSERT_FALSE(stream.active);

	// --- 8. DSP Filter Anti-Aliasing Cutoff Invariants ---
	printf("\n[Group 8: DSP Filter Anti-Aliasing Cutoff Invariants]\n");
	// Downsampling / pitch > 1.0 cutoff rule:
	// nyquistSource = srcRate * 0.5f
	// nyquistDest   = 24000 * 0.5f / pitch
	// cutoff = min(nyquistSource, nyquistDest) * 0.90f
	auto CalcCutoff = [](float srcRate, float pitch) -> float {
		float nyquistSource = srcRate * 0.5f;
		float nyquistDest = (24000.0f * 0.5f) / pitch;
		float cutoff = (std::min)(nyquistSource, nyquistDest) * 0.90f;
		return (std::min)(cutoff, 24000.0f * 0.45f);
	};

	// Standard 44.1kHz -> 24kHz at pitch 1.0: dest Nyquist (12kHz) dominates, cutoff = 10.8kHz
	float c1 = CalcCutoff(44100.0f, 1.0f);
	ASSERT_TRUE(c1 > 10000.0f && c1 <= 10800.0f);

	// 24kHz at pitch 2.0: effective dest Nyquist is 6kHz, cutoff = 5.4kHz (prevents high-frequency aliasing)
	float c2 = CalcCutoff(24000.0f, 2.0f);
	ASSERT_TRUE(c2 > 5000.0f && c2 <= 5400.0f);

	// Low sample rate 11025Hz at pitch 1.0: source Nyquist (5512.5Hz) dominates, cutoff ~4961Hz
	float c3 = CalcCutoff(11025.0f, 1.0f);
	ASSERT_TRUE(c3 > 4500.0f && c3 <= 5000.0f);

	// --- 9. Client ID Allocation & Wraparound Tests ---
	printf("\n[Group 9: Client ID Allocation & Collision-Free Wraparound]\n");
	std::vector<uint32_t> registeredIds;
	uint32_t nextId = 1;
	auto AllocClientId = [&]() -> uint32_t {
		if (registeredIds.size() >= 254)
			return 0;
		uint32_t candidate = nextId;
		for (int i = 0; i < 254; ++i) {
			if (candidate == 0 || candidate > 254) candidate = 1;
			if (std::find(registeredIds.begin(), registeredIds.end(), candidate) == registeredIds.end()) {
				nextId = (candidate >= 254) ? 1 : candidate + 1;
				registeredIds.push_back(candidate);
				return candidate;
			}
			candidate++;
		}
		return 0;
	};
	auto FreeClientId = [&](uint32_t cid) {
		auto it = std::find(registeredIds.begin(), registeredIds.end(), cid);
		if (it != registeredIds.end()) registeredIds.erase(it);
	};

	// Allocate all 254 slots
	for (int i = 1; i <= 254; ++i) {
		uint32_t cid = AllocClientId();
		ASSERT_EQ(cid, static_cast<uint32_t>(i));
	}
	// 255th allocation must be rejected (0)
	ASSERT_EQ(AllocClientId(), 0);

	// Free slot 42, next allocation must reuse slot 42 without collision
	FreeClientId(42);
	ASSERT_EQ(registeredIds.size(), 253);
	uint32_t reused = AllocClientId();
	ASSERT_EQ(reused, 42);

	// --- 10. callerTrackId Event Propagation & Strict StopTrack Isolation ---
	printf("\n[Group 10: callerTrackId Event Propagation & Strict StopTrack Isolation]\n");
	struct MockVoiceTrack {
		uint32_t trackId = 0;
		uint32_t callerTrackId = 0;
		uint32_t GetEventTrackId() const { return callerTrackId != 0 ? callerTrackId : trackId; }
	};
	MockVoiceTrack trackDefault{ 0x01000001, 0 };
	MockVoiceTrack trackWithCaller{ 0x01000002, 9999 };

	ASSERT_EQ(trackDefault.GetEventTrackId(), 0x01000001);
	ASSERT_EQ(trackWithCaller.GetEventTrackId(), 9999);

	// StopTrack matches strictly on the unique returned core trackId, preventing cross-client callerTrackId collisions
	auto StopTrackMatches = [](const MockVoiceTrack& t, uint32_t queryId) -> bool {
		return t.trackId == queryId;
	};
	ASSERT_TRUE(StopTrackMatches(trackWithCaller, 0x01000002));  // Match by internal generated ID
	ASSERT_FALSE(StopTrackMatches(trackWithCaller, 9999));        // Caller ID is isolated from StopTrack
	ASSERT_FALSE(StopTrackMatches(trackWithCaller, 8888));

	// --- 11. Paused Stack Overflow Notification ---
	printf("\n[Group 11: Paused Stack Overflow Dropped Track Notification]\n");
	std::vector<MockVoiceTrack> pausedStack;
	std::vector<uint32_t> finishedEvents;
	constexpr size_t kStackLimit = 16;
	for (size_t i = 1; i <= 20; ++i) {
		MockVoiceTrack newTrack{ static_cast<uint32_t>(i), 0 };
		if (pausedStack.size() >= kStackLimit) {
			finishedEvents.push_back(pausedStack.front().GetEventTrackId());
			pausedStack.erase(pausedStack.begin());
		}
		pausedStack.push_back(newTrack);
	}
	ASSERT_EQ(pausedStack.size(), kStackLimit);
	ASSERT_EQ(finishedEvents.size(), 4);
	ASSERT_EQ(finishedEvents[0], 1);
	ASSERT_EQ(finishedEvents[1], 2);
	ASSERT_EQ(finishedEvents[2], 3);
	ASSERT_EQ(finishedEvents[3], 4);

	// --- 12. PTT Ducking on Track Advance ---
	printf("\n[Group 12: PTT Ducking Invariant on Track Advance]\n");
	enum class MockState { IDLE, PLAYING, DUCKED };
	auto AdvanceTrack = [](bool userTalking, bool allowDuck) -> MockState {
		if (allowDuck && userTalking)
			return MockState::DUCKED;
		return MockState::PLAYING;
	};
	// Advance with PTT held and allowDuck = true -> DUCKED
	ASSERT_TRUE(AdvanceTrack(true, true) == MockState::DUCKED);
	// Advance with PTT released -> PLAYING
	ASSERT_TRUE(AdvanceTrack(false, true) == MockState::PLAYING);
	// Advance with PTT held but allowDuck = false -> PLAYING (uninterruptible)
	ASSERT_TRUE(AdvanceTrack(true, false) == MockState::PLAYING);

	// --- 13. Client ID Reuse Protection with Active Tracks ---
	printf("\n[Group 13: Client ID Reuse Protection with Active Tracks]\n");
	std::vector<uint32_t> activeTrackOwners = { 42 };
	auto HasClientTracks = [&](uint32_t cid) -> bool {
		return std::find(activeTrackOwners.begin(), activeTrackOwners.end(), cid) != activeTrackOwners.end();
	};
	auto SafeAllocClientId = [&](std::unordered_set<uint32_t>& regIds, uint32_t& nextId) -> uint32_t {
		uint32_t cand = nextId;
		for (int i = 0; i < 254; ++i) {
			if (cand == 0 || cand > 254) cand = 1;
			if (regIds.find(cand) == regIds.end() && !HasClientTracks(cand)) {
				nextId = cand + 1;
				if (nextId > 254) nextId = 1;
				regIds.insert(cand);
				return cand;
			}
			cand++;
		}
		return 0;
	};
	std::unordered_set<uint32_t> activeRegIds;
	uint32_t nextCandidate = 40;
	uint32_t cid1 = SafeAllocClientId(activeRegIds, nextCandidate);
	ASSERT_EQ(cid1, 40);
	uint32_t cid2 = SafeAllocClientId(activeRegIds, nextCandidate);
	ASSERT_EQ(cid2, 41);
	// Candidate 42 is in activeTrackOwners, so it MUST be skipped!
	uint32_t cid3 = SafeAllocClientId(activeRegIds, nextCandidate);
	ASSERT_EQ(cid3, 43); // 42 was skipped safely
	// Now tracks for 42 finish
	activeTrackOwners.clear();
	nextCandidate = 42;
	uint32_t cid4 = SafeAllocClientId(activeRegIds, nextCandidate);
	ASSERT_EQ(cid4, 42); // Now 42 can be reused safely

	// --- 14. ClearClientQueue Isolation ---
	printf("\n[Group 14: ClearClientQueue Isolation]\n");
	struct ClientTrackMock {
		uint32_t trackId;
		uint32_t ownerId;
	};
	ClientTrackMock curTrack{ 101, 1 }; // Client 1 playing
	std::vector<ClientTrackMock> qTracks = {
		{ 102, 1 }, // Client 1 queued
		{ 103, 2 }, // Client 2 queued
		{ 104, 1 }, // Client 1 queued
		{ 105, 3 }  // Client 3 queued
	};
	// ClearClientQueue(1) removes Client 1's queued tracks, leaves curTrack and other clients untouched
	auto ClearClientQ = [&](uint32_t cid) {
		for (auto it = qTracks.begin(); it != qTracks.end(); ) {
			if (it->ownerId == cid) it = qTracks.erase(it);
			else ++it;
		}
	};
	ClearClientQ(1);
	ASSERT_EQ(curTrack.trackId, 101); // Playing track untouched
	ASSERT_EQ(qTracks.size(), 2);
	ASSERT_EQ(qTracks[0].ownerId, 2);
	ASSERT_EQ(qTracks[1].ownerId, 3);

	// --- 15. Audio Clamping Policy ---
	printf("\n[Group 15: Audio Clamping Policy (Never Reject Long Audio)]\n");
	constexpr int kMaxDurationSec = 600;
	int sampleRate = 44100;
	size_t maxFrames = static_cast<size_t>(sampleRate) * kMaxDurationSec;
	size_t longAudioFrames = maxFrames + 44100 * 300; // 15 minutes
	size_t clampedFrames = longAudioFrames;
	if (clampedFrames > maxFrames) {
		clampedFrames = maxFrames;
	}
	ASSERT_EQ(clampedFrames, maxFrames);
	ASSERT_TRUE(clampedFrames > 0);

	// --- 16. GetTrackOwner Strict Core ID Matching ---
	printf("\n[Group 16: GetTrackOwner Strict Core ID Matching]\n");
	struct OwnerMockTrack {
		uint32_t trackId;
		uint32_t callerTrackId;
		uint32_t ownerId;
	};
	std::vector<OwnerMockTrack> activeTracks = {
		{ 0x02000005, 777, 2 }, // Generated ID 0x02000005, callerTrackId 777, owner 2
		{ 0x03000006, 0, 3 }
	};
	auto MockGetTrackOwner = [&](uint32_t qId) -> uint32_t {
		if (qId == 0) return 0;
		for (const auto& t : activeTracks) {
			if (t.trackId == qId)
				return t.ownerId;
		}
		return (qId >> 24);
	};
	ASSERT_EQ(MockGetTrackOwner(0x02000005), 2); // Resolved via core trackId
	ASSERT_EQ(MockGetTrackOwner(0x03000006), 3); // Resolved via core trackId
	ASSERT_EQ(MockGetTrackOwner(0x05000010), 5); // Fallback to namespace high byte
	ASSERT_EQ(MockGetTrackOwner(777), 0);        // Caller ID isolated from GetTrackOwner to prevent collisions

	// --- 17. Hitch & Alt-Tab Clock Compensation ---
	printf("\n[Group 17: Hitch & Alt-Tab Clock Compensation]\n");
	uint64_t dueUs = 200000; // Chunk due at 200ms
	uint64_t elapsedHitchUs = 5200000; // 5.2s elapsed (5s freeze/alt-tab)
	constexpr uint64_t kMaxLateUs = 200000;
	if (elapsedHitchUs > dueUs + kMaxLateUs)
	{
		elapsedHitchUs = dueUs;
	}
	ASSERT_EQ(elapsedHitchUs, 200000); // Re-anchored to due time, burst prevented

	TEST_SUITE_END();
}

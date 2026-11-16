#pragma once

#include <metahook.h>
#include <vector>
#include <string>

// Header structure for NamedPipe binary stream packets
enum VoicePipeCommand : uint16_t
{
	PIPE_CMD_PLAY_FILE    = 1,
	PIPE_CMD_PLAY_PCM     = 2,
	PIPE_CMD_STREAM_CHUNK = 3,
	PIPE_CMD_STOP         = 4,
	PIPE_CMD_PAUSE        = 5,
	PIPE_CMD_STREAM_END   = 6, // Closes the stream owned by this connection
	PIPE_CMD_CLOSE_STREAM = 6, // Backward-compatible alias for STREAM_END
	PIPE_CMD_RESUME       = 7
};

#pragma pack(push, 1)
struct VoicePipeHeader
{
	uint32_t magic;         // 0x56455854 ("VEXT")
	uint16_t command;       // VoicePipeCommand (1=PLAY_FILE, 2=PLAY_PCM, 3=STREAM_CHUNK, 4=STOP, 5=PAUSE, 6=STREAM_END, 7=RESUME)
	uint16_t sampleRate;    // e.g. 16000, 24000, 44100, 48000
	uint8_t  channels;      // 1 = mono, 2 = stereo
	uint8_t  priority;      // VoicePriority (0=PASSIVE, 1=NORMAL, 2=HIGH, 3=URGENT)
	float    volume;        // 0.0 .. 2.0 (clamped)
	float    pitch;         // 0.5 .. 2.0 (clamped)
	uint32_t dataBytes;     // Exact length of payload following this header
};
#pragma pack(pop)

constexpr uint32_t VOICE_PIPE_MAGIC = 0x56455854; // "VEXT"

void VoicePipe_StartServer();
void VoicePipe_StopServer();

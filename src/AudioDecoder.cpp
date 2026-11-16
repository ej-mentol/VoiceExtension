#include "AudioDecoder.h"
#include "DebugLog.h"
#include <windows.h>
#include <fstream>
#include <cstring>
#include <cmath>
#include <metahook.h>
#include <Interface/IFileSystem.h>

extern mh_interface_t* g_pInterface;

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

// Include stb_vorbis implementation
#include "stb_vorbis.c"

bool AudioDecoder::DecodeFile(const char* fullPath, DecodedAudio& outAudio)
{
	if (!fullPath || !fullPath[0])
		return false;

	DebuLog("AudioDecoder::DecodeFile: '%s'", fullPath);

	std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
	if (!file.is_open())
	{
		// Fallback to engine's IFileSystem in case the file is in an engine search path
		IFileSystem* pFS = g_pInterface ? g_pInterface->FileSystem : nullptr;
		if (!pFS && g_pInterface)
			pFS = reinterpret_cast<IFileSystem*>(g_pInterface->FileSystem_HL25);

		if (pFS)
		{
			FileHandle_t hFile = pFS->Open(fullPath, "rb", nullptr);
			if (!hFile)
				hFile = pFS->Open(fullPath, "rb", "GAME");

			if (hFile)
			{
				unsigned sz = pFS->Size(hFile);
				if (sz > 16 && sz <= 64 * 1024 * 1024)
				{
					std::vector<uint8_t> buffer(sz);
					if (pFS->Read(buffer.data(), sz, hFile) == static_cast<int>(sz))
					{
						pFS->Close(hFile);
						DebuLog("AudioDecoder::DecodeFile: read %u bytes via IFileSystem", sz);
						return DecodeMemory(buffer.data(), sz, outAudio);
					}
				}
				pFS->Close(hFile);
			}
		}
		DebuLog("AudioDecoder::DecodeFile: failed to open '%s'", fullPath);
		return false;
	}

	std::streamsize size = file.tellg();
	if (size <= 16)
	{
		DebuLog("AudioDecoder::DecodeFile: file too small (%lld bytes)", static_cast<long long>(size));
		return false;
	}

	// Reject files larger than 64 MB. A 64 MB WAV at 24 kHz mono 16-bit is ~22 minutes,
	// well above any reasonable in-game clip. This prevents OOM from crafted inputs.
	constexpr std::streamsize kMaxFileBytes = 64 * 1024 * 1024;
	if (size > kMaxFileBytes)
	{
		DebuLog("AudioDecoder::DecodeFile: file exceeds 64MB limit (%lld bytes)", static_cast<long long>(size));
		return false;
	}

	file.seekg(0, std::ios::beg);
	std::vector<uint8_t> buffer(static_cast<size_t>(size));
	if (!file.read(reinterpret_cast<char*>(buffer.data()), size))
	{
		DebuLog("AudioDecoder::DecodeFile: failed to read %lld bytes", static_cast<long long>(size));
		return false;
	}

	DebuLog("AudioDecoder::DecodeFile: read %zu bytes from disk", buffer.size());
	return DecodeMemory(buffer.data(), buffer.size(), outAudio);
}

bool AudioDecoder::DecodeMemory(const uint8_t* data, size_t dataSize, DecodedAudio& outAudio)
{
	outAudio.monoSamples.clear();
	outAudio.sampleRate = 0;

	if (!data || dataSize <= 16)
		return false;

	constexpr size_t kMaxFileBytes = 64 * 1024 * 1024;
	if (dataSize > kMaxFileBytes)
		return false;

	DebuLog("AudioDecoder::DecodeMemory: inspecting header (%zu bytes)", dataSize);

	// Format detection by magic bytes
	if (memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0)
	{
		DebuLog("AudioDecoder::DecodeMemory: identified as WAV container");
		if (!DecodeWav(data, dataSize, outAudio))
			return false;
	}
	else if (memcmp(data, "OggS", 4) == 0)
	{
		DebuLog("AudioDecoder::DecodeMemory: identified as OGG container");
		if (!DecodeOgg(data, dataSize, outAudio))
			return false;
	}
	else
	{
		// Try MP3 (handles ID3 tags or raw MPEG sync headers)
		DebuLog("AudioDecoder::DecodeMemory: attempting MP3 decoding");
		if (!DecodeMp3(data, dataSize, outAudio))
		{
			// Fallback: check if OGG Vorbis without standard first page
			DebuLog("AudioDecoder::DecodeMemory: MP3 failed, attempting OGG fallback");
			if (!DecodeOgg(data, dataSize, outAudio))
			{
				DebuLog("AudioDecoder::DecodeMemory: unrecognized audio format");
				return false;
			}
		}
	}

	// Clamp decoded audio that exceeds the duration limit dynamically scaled to sample rate
	constexpr int kMaxDurationSeconds = 600; // 10 minutes
	if (outAudio.sampleRate > 0)
	{
		const size_t maxSamples = (outAudio.sampleRate <= 192000)
			? static_cast<size_t>(outAudio.sampleRate) * kMaxDurationSeconds
			: static_cast<size_t>(48000) * kMaxDurationSeconds;
		if (outAudio.monoSamples.size() > maxSamples)
		{
			DebuLog("AudioDecoder::DecodeMemory: clamping decoded audio from %zu to %zu samples (%d seconds max)",
				outAudio.monoSamples.size(), maxSamples, kMaxDurationSeconds);
			outAudio.monoSamples.resize(maxSamples);
		}
	}

	return true;
}

bool AudioDecoder::DecodeWav(const uint8_t* data, size_t size, DecodedAudio& outAudio)
{
	if (size < 44)
		return false;

	uint16_t audioFormat = 0;
	uint16_t numChannels = 0;
	uint32_t sampleRate = 0;
	uint16_t bitsPerSample = 0;
	const uint8_t* audioData = nullptr;
	uint32_t audioDataSize = 0;

	size_t offset = 12; // skip RIFF + size + WAVE

	while (offset + 8 <= size)
	{
		char chunkId[4];
		memcpy(chunkId, data + offset, 4);
		offset += 4;

		uint32_t chunkSize = 0;
		memcpy(&chunkSize, data + offset, sizeof(uint32_t));
		offset += 4;

		// 32-bit integer overflow protection: chunkSize must not exceed remaining file bytes
		if (chunkSize > size - offset)
		{
			if (memcmp(chunkId, "data", 4) == 0)
			{
				audioData = data + offset;
				audioDataSize = static_cast<uint32_t>(size - offset);
			}
			break;
		}

		size_t paddedSize = static_cast<size_t>(chunkSize) + (chunkSize & 1);
		if (paddedSize > size - offset)
		{
			paddedSize = size - offset;
		}

		if (memcmp(chunkId, "fmt ", 4) == 0)
		{
			if (chunkSize < 16 || size - offset < 16)
				return false;

			memcpy(&audioFormat, data + offset, sizeof(uint16_t));
			memcpy(&numChannels, data + offset + 2, sizeof(uint16_t));
			memcpy(&sampleRate, data + offset + 4, sizeof(uint32_t));
			memcpy(&bitsPerSample, data + offset + 14, sizeof(uint16_t));

			// Support WAVE_FORMAT_EXTENSIBLE (0xFFFE) with PCM / IEEE_FLOAT subformat GUIDs
			if (audioFormat == 0xFFFE && chunkSize >= 40 && size - offset >= 40)
			{
				uint32_t subFormatData1 = 0;
				memcpy(&subFormatData1, data + offset + 24, sizeof(uint32_t));
				if (subFormatData1 == 1) // KSDATAFORMAT_SUBTYPE_PCM
				{
					audioFormat = 1;
				}
				else if (subFormatData1 == 3) // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
				{
					audioFormat = 3;
				}
			}
		}
		else if (memcmp(chunkId, "data", 4) == 0)
		{
			audioData = data + offset;
			audioDataSize = chunkSize;
			break;
		}

		offset += paddedSize;
	}

	if (!audioData || audioDataSize == 0 || numChannels == 0 || numChannels > 2 || sampleRate == 0 || sampleRate > 192000)
		return false;
	if (audioFormat != 1 && audioFormat != 3) // PCM (1) or IEEE Float (3)
		return false;
	if (audioFormat == 3 && bitsPerSample != 32)
		return false;
	if (audioFormat == 1 && (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32))
		return false;

	const size_t bytesPerSample = bitsPerSample / 8;
	const size_t bytesPerFrame = bytesPerSample * numChannels;
	size_t totalFrames = audioDataSize / bytesPerFrame;
	if (totalFrames == 0)
		return false;

	const size_t kMaxSamples = static_cast<size_t>(sampleRate) * 600; // 10 minutes max dynamically scaled to rate
	if (totalFrames > kMaxSamples)
	{
		DebuLog("AudioDecoder::DecodeWav: clamping WAV from %zu to %zu frames (10 minutes max)",
			totalFrames, kMaxSamples);
		totalFrames = kMaxSamples;
	}

	try
	{
		outAudio.monoSamples.resize(totalFrames);
	}
	catch (const std::bad_alloc&)
	{
		return false;
	}
	outAudio.sampleRate = static_cast<int>(sampleRate);

	const uint8_t* ptr = audioData;
	for (size_t f = 0; f < totalFrames; ++f)
	{
		float sum = 0.0f;
		for (uint16_t c = 0; c < numChannels; ++c)
		{
			float val = 0.0f;
			if (audioFormat == 1) // PCM
			{
				switch (bitsPerSample)
				{
				case 8:
				{
					uint8_t u8 = *ptr++;
					val = (static_cast<float>(u8) - 128.0f) / 128.0f;
					break;
				}
				case 16:
				{
					int16_t s16 = 0;
					memcpy(&s16, ptr, sizeof(int16_t));
					ptr += 2;
					val = static_cast<float>(s16) / 32768.0f;
					break;
				}
				case 24:
				{
					int32_t s24 = static_cast<int32_t>((ptr[0] | (ptr[1] << 8) | (ptr[2] << 16)));
					if (s24 & 0x800000) s24 |= 0xFF000000;
					ptr += 3;
					val = static_cast<float>(s24) / 8388608.0f;
					break;
				}
				case 32:
				default:
				{
					int32_t s32 = 0;
					memcpy(&s32, ptr, sizeof(int32_t));
					ptr += 4;
					val = static_cast<float>(s32) / 2147483648.0f;
					break;
				}
				}
			}
			else // audioFormat == 3, IEEE Float (32-bit)
			{
				float fVal = 0.0f;
				memcpy(&fVal, ptr, sizeof(float));
				val = fVal;
				ptr += 4;
			}
			sum += val;
		}
		outAudio.monoSamples[f] = sum / static_cast<float>(numChannels);
	}

	return true;
}

bool AudioDecoder::DecodeMp3(const uint8_t* data, size_t size, DecodedAudio& outAudio)
{
	mp3dec_t mp3d;
	mp3dec_init(&mp3d);

	mp3dec_frame_info_t info = {};
	int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

	size_t offset = 0;
	int sampleRate = 0;
	int channels = 0;

	// Conservative reservation to prevent 32-bit virtual memory exhaustion.
	// 1 minute of 48kHz audio = 48000 samples (~192 KB), grows dynamically as needed.
	std::vector<float> mono;
	try
	{
		mono.reserve(std::min<size_t>(size, 48000 * 60));
	}
	catch (const std::bad_alloc&)
	{
		return false;
	}

	size_t maxSamples = static_cast<size_t>(48000) * 600;

	try
	{
		while (offset < size)
		{
			int samples = mp3dec_decode_frame(&mp3d, data + offset, static_cast<int>(size - offset), pcm, &info);
			offset += info.frame_bytes;

			if (samples > 0)
			{
				if (sampleRate == 0)
				{
					sampleRate = info.hz;
					channels = info.channels;
					if (sampleRate <= 0 || sampleRate > 192000 || channels <= 0 || channels > 2)
						return false;
					maxSamples = static_cast<size_t>(sampleRate) * 600;
				}
				else if (info.hz != sampleRate || info.channels != channels)
				{
					// Disallow mid-stream audio format/channel changes
					break;
				}

				if (info.channels == 1)
				{
					for (int i = 0; i < samples; ++i)
					{
						mono.push_back(static_cast<float>(pcm[i]) / 32768.0f);
					}
				}
				else if (info.channels == 2)
				{
					for (int i = 0; i < samples; ++i)
					{
						float l = static_cast<float>(pcm[i * 2]) / 32768.0f;
						float r = static_cast<float>(pcm[i * 2 + 1]) / 32768.0f;
						mono.push_back((l + r) * 0.5f);
					}
				}
				if (mono.size() > maxSamples)
					break;
			}

			if (info.frame_bytes == 0)
				break;
		}
	}
	catch (const std::bad_alloc&)
	{
		return false;
	}

	if (mono.size() > maxSamples)
	{
		DebuLog("AudioDecoder::DecodeMp3: clamping MP3 from %zu to %zu samples (10 minutes max)",
			mono.size(), maxSamples);
		mono.resize(maxSamples);
	}

	if (mono.empty() || sampleRate <= 0)
		return false;

	outAudio.monoSamples = std::move(mono);
	outAudio.sampleRate = sampleRate;
	return true;
}

bool AudioDecoder::DecodeOgg(const uint8_t* data, size_t size, DecodedAudio& outAudio)
{
	if (!data || size == 0)
		return false;

	int error = 0;
	stb_vorbis* v = stb_vorbis_open_memory(data, static_cast<int>(size), &error, nullptr);
	if (!v)
		return false;

	stb_vorbis_info info = stb_vorbis_get_info(v);
	if (info.channels <= 0 || info.channels > 2 || info.sample_rate <= 0 || info.sample_rate > 192000)
	{
		stb_vorbis_close(v);
		return false;
	}

	// Bounded reservation to prevent excessive 32-bit address space allocation
	unsigned int streamLength = stb_vorbis_stream_length_in_samples(v);
	const size_t kMaxSamples = static_cast<size_t>(info.sample_rate) * 600; // 10 minutes limit dynamically scaled to rate

	outAudio.sampleRate = static_cast<int>(info.sample_rate);
	outAudio.monoSamples.clear();

	const size_t initialCapacity = (streamLength > 0 && streamLength != 0xFFFFFFFF && streamLength <= kMaxSamples) ? streamLength : 48000;
	try
	{
		outAudio.monoSamples.reserve(std::min<size_t>(initialCapacity, kMaxSamples));
	}
	catch (const std::bad_alloc&)
	{
		stb_vorbis_close(v);
		return false;
	}

	constexpr int kChunkFrames = 4096;
	std::vector<short> chunkBuffer(info.channels * kChunkFrames);

	while (outAudio.monoSamples.size() < kMaxSamples)
	{
		int n = stb_vorbis_get_samples_short_interleaved(v, info.channels, chunkBuffer.data(), info.channels * kChunkFrames);
		if (n <= 0)
			break;

		size_t toAdd = static_cast<size_t>(n);
		if (outAudio.monoSamples.size() + toAdd > kMaxSamples)
		{
			toAdd = kMaxSamples - outAudio.monoSamples.size();
			DebuLog("AudioDecoder::DecodeOgg: clamping OGG from %zu to %zu samples (10 minutes max)",
				outAudio.monoSamples.size() + toAdd, kMaxSamples);
		}

		size_t prevSize = outAudio.monoSamples.size();
		try
		{
			outAudio.monoSamples.resize(prevSize + toAdd);
		}
		catch (const std::bad_alloc&)
		{
			stb_vorbis_close(v);
			return false;
		}

		if (info.channels == 1)
		{
			for (size_t i = 0; i < toAdd; ++i)
			{
				outAudio.monoSamples[prevSize + i] = static_cast<float>(chunkBuffer[i]) / 32768.0f;
			}
		}
		else
		{
			for (size_t i = 0; i < toAdd; ++i)
			{
				float sum = 0.0f;
				for (int c = 0; c < info.channels; ++c)
				{
					sum += static_cast<float>(chunkBuffer[i * info.channels + c]) / 32768.0f;
				}
				outAudio.monoSamples[prevSize + i] = sum / static_cast<float>(info.channels);
			}
		}

		if (toAdd < static_cast<size_t>(n))
			break;
	}

	stb_vorbis_close(v);

	return !outAudio.monoSamples.empty();
}

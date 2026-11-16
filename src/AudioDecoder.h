#pragma once

#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include <vector>
#include <cstdint>
#include <string>

struct DecodedAudio
{
	std::vector<float> monoSamples; // Normalized to [-1.0 .. +1.0] mono
	int sampleRate = 0;
};

class AudioDecoder
{
public:
	static bool DecodeFile(const char* fullPath, DecodedAudio& outAudio);
	static bool DecodeMemory(const uint8_t* data, size_t dataSize, DecodedAudio& outAudio);

private:
	static bool DecodeWav(const uint8_t* data, size_t size, DecodedAudio& outAudio);
	static bool DecodeMp3(const uint8_t* data, size_t size, DecodedAudio& outAudio);
	static bool DecodeOgg(const uint8_t* data, size_t size, DecodedAudio& outAudio);
};

#endif // AUDIO_DECODER_H

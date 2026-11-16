#pragma once

#include <string>
#include <cmath>
#include <cctype>
#include <cstring>

namespace VoiceUtils
{

inline float SoftLimit(float x)
{
	constexpr float thresh = 0.707f;
	if (x > thresh)
	{
		const float excess = x - thresh;
		return thresh + (1.0f - thresh) * std::tanh(excess / (1.0f - thresh));
	}
	else if (x < -thresh)
	{
		const float excess = -x - thresh;
		return -(thresh + (1.0f - thresh) * std::tanh(excess / (1.0f - thresh)));
	}
	return x;
}

inline bool IsPathSafe(const char* path, bool isFromPipe, bool allowAbsPaths)
{
	if (!path || !path[0])
		return false;

	// Check for control characters (ASCII < 32) and invalid path/stream characters
	for (const char* p = path; *p; ++p)
	{
		const unsigned char uc = static_cast<unsigned char>(*p);
		if (uc < 32 || *p == '"' || *p == '<' || *p == '>' || *p == '|' || *p == '?' || *p == '*')
			return false;
	}

	std::string s = path;

	// Block directory traversal
	if (s.find("..") != std::string::npos)
		return false;

	// Check if path is absolute with Windows drive letter (e.g. C:/ or C:\)
	const bool isAbsolute = (s.size() >= 3 && isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':' && (s[2] == '/' || s[2] == '\\'));

	if (isAbsolute)
	{
		// Absolute paths are prohibited from console/commands/plugins to preserve sandbox.
		// They are only permitted when delivered via local pipe AND explicitly allowed.
		if (!isFromPipe || !allowAbsPaths)
			return false;

		// Prevent NTFS Alternate Data Streams (e.g. C:/test.wav:stream)
		if (s.find(':', 2) != std::string::npos)
			return false;
	}
	else
	{
		// Relative path: reject colons, root-relative slashes, and device prefixes
		if (s.find(':') != std::string::npos || s[0] == '/' || s[0] == '\\')
			return false;
	}

	for (char& c : s)
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

	// Whitelist: only allow known safe audio extensions
	const bool hasGoodExt = (s.size() > 4) &&
		(s.rfind(".wav") == s.size() - 4 ||
		 s.rfind(".mp3") == s.size() - 4 ||
		 s.rfind(".ogg") == s.size() - 4);
	if (!hasGoodExt)
		return false;

	// Extract bare filename and reject engine-reserved names and DOS devices
	const size_t lastSlash = s.find_last_of("/\\");
	const std::string filename = (lastSlash != std::string::npos) ? s.substr(lastSlash + 1) : s;

	if (filename == "voice_micdata.wav" || filename == "voice_decompressed.wav" || filename == "voice_input.wav")
		return false;

	const size_t dotPos = filename.rfind('.');
	const std::string stem = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
	static const char* const kDosDevices[] = {
		"con", "prn", "aux", "nul",
		"com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
		"lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
	};
	for (const char* dev : kDosDevices)
	{
		if (stem == dev)
			return false;
	}

	return true;
}

} // namespace VoiceUtils

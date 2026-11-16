#pragma once

#ifndef VOICE_EXTENSION_PLUGINS_H
#define VOICE_EXTENSION_PLUGINS_H

#include <metahook.h>
#include <cvardef.h>
#include <Interface/IPlugins.h>
#include "Interface/IVoiceExtension.h"

#define MHPluginName "VoiceExtension"

extern cl_enginefunc_t gEngfuncs;
extern cl_exportfuncs_t gExportfuncs;
extern metahook_api_t* g_pMetaHookAPI;
extern mh_interface_t* g_pInterface;
extern mh_enginesave_t* g_pMetaSave;

// Voice datagram buffer pointers discovered from hw.dll's Voice_Send
extern uint8_t* volatile* g_voiceSendData;
extern volatile uint32* g_voiceSendCursor;
extern volatile uint32* g_voiceSendMaxSize;
extern volatile int* g_clientStatePtr;

// ConVars
extern cvar_t* g_ve_allow_abs_paths;
extern cvar_t* g_ve_allow_server;
extern cvar_t* g_ve_debug;

inline bool VE_IsDebug() { return g_ve_debug && g_ve_debug->value > 0.0f; }
#define VE_DEBUG_LOG(...) do { if (g_ve_debug && g_ve_debug->value > 0.0f) SafeConPrintf(__VA_ARGS__); } while (0)

void SafeConPrintf(const char* fmt, ...);

class CVoiceExtension : public IVoiceExtension3
{
public:
	// IVoiceExtension
	uint32_t EnqueueFile(const char* relativePath, const VoicePlayParams& params = {}) override;
	uint32_t EnqueuePcm(const int16_t* pcm, size_t samples, int sampleRate, const VoicePlayParams& params = {}) override;
	
	void StopAll() override;
	void StopCurrent() override;
	void ClearQueue() override;
	void Pause(bool pause) override;

	bool OpenStream(int sampleRate, const VoicePlayParams& params = {}) override;
	bool PushStreamPcm(const int16_t* pcm, size_t samples) override;
	void CloseStream() override;

	VoicePlaybackState GetState() const override;
	bool IsTransmitting() const override;
	size_t GetQueueSize() const override;
	uint64_t GetCurrentSteamID() const override;

	void RegisterListener(IVoiceEventListener* listener) override;
	void UnregisterListener(IVoiceEventListener* listener) override;

	// IVoiceExtension2
	bool IsPanic() const override;
	const char* GetPanicReason() const override;
	bool IsReady() const override;

	// IVoiceExtension3
	uint32_t RegisterClient(const char* clientName, const VoiceClientParams& params = VoiceClientParams()) override;
	void UnregisterClient(uint32_t clientId) override;
	uint32_t EnqueueFileEx(uint32_t clientId, const char* relativePath, const VoicePlayParams& params = {}) override;
	uint32_t EnqueuePcmEx(uint32_t clientId, const int16_t* pcm, size_t samples, int sampleRate, const VoicePlayParams& params = {}) override;
	bool OpenStreamEx(uint32_t clientId, int sampleRate, const VoicePlayParams& params = {}) override;
	bool PushStreamPcmEx(uint32_t clientId, const int16_t* pcm, size_t samples) override;
	void CloseStreamEx(uint32_t clientId) override;
	bool IsStreamingActiveEx(uint32_t clientId) const override;
	uint32_t GetStreamOwner() const override;
	bool StopTrack(uint32_t trackId) override;
	void StopClient(uint32_t clientId) override;
	size_t GetClientQueueSize(uint32_t clientId) const override;
	uint32_t GetTrackOwner(uint32_t trackId) const override;
};

extern CVoiceExtension g_VoiceExtension;

#endif // VOICE_EXTENSION_PLUGINS_H

#include <metahook.h>
#include <cvardef.h>
#include <strsafe.h>
#include <string>
#include <vector>
#include "plugins.h"
#include "VoiceCore.h"
#include "VoicePipeServer.h"
#include "DebugLog.h"

cl_enginefunc_t gEngfuncs = {};
cl_exportfuncs_t gExportfuncs = {};
metahook_api_t* g_pMetaHookAPI = nullptr;
mh_interface_t* g_pInterface = nullptr;
mh_enginesave_t* g_pMetaSave = nullptr;

uint8_t* volatile* g_voiceSendData = nullptr;
volatile uint32* g_voiceSendCursor = nullptr;
volatile uint32* g_voiceSendMaxSize = nullptr;
volatile int* g_clientStatePtr = nullptr;

// Configurable duck-resume debounce delay (milliseconds).
// Prevents the plugin track from resuming immediately after the user releases
// the microphone key, giving time for key-bounce to settle.
cvar_t* g_ve_duck_resume_ms = nullptr;

// Security sandbox cvar: when set to 0 (default), all absolute paths (e.g. C:/...)
// are rejected to prevent path traversal or unauthorized local file access.
// When set to 1, absolute paths are allowed strictly via named pipe (e.g. for external TTS daemons).
cvar_t* g_ve_allow_abs_paths = nullptr;

// Security gate cvar: when set to 0 (default), server-triggered playback via stuffcmd is ignored.
cvar_t* g_ve_allow_server = nullptr;

// Debug logging cvar: when 0 (default), all routine spam and debug logs are silenced.
cvar_t* g_ve_debug = nullptr;

void SafeConPrintf(const char* fmt, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	buffer[sizeof(buffer) - 1] = '\0';

	if (gEngfuncs.Con_Printf)
	{
		gEngfuncs.Con_Printf("%s", buffer);
	}
	else
	{
		OutputDebugStringA(buffer);
	}
}

namespace
{
	constexpr ULONG_PTR kVoiceSendCursorRva = 0x3F8300;
	constexpr ULONG_PTR kVoiceSendMaxSizeRva = 0x3F82FC;
	constexpr ULONG_PTR kVoiceSendDataRva = kVoiceSendMaxSizeRva - sizeof(void*);
	constexpr uint32_t kVoiceSendExpectedMaxSize = 0xFFCF;

	const char kVoiceSendSignature[] =
		"\x81\xEC\x04\x08\x00\x00\xA1\x2A\x2A\x2A\x2A\x33\xC4\x89\x84\x24\x00\x08\x00\x00"
		"\x83\x3D\x2A\x2A\x2A\x2A\x05\x75\x6A\xE8\x2A\x2A\x2A\x2A\x85\xC0\x74\x61\x56"
		"\xFF\xB4\x24\x0C\x08\x00\x00\x8D\x44";

	void(__cdecl* g_originalVoiceSend)(int) = nullptr;
	hook_t* g_voiceSendHook = nullptr;

	using VoiceRecordingControlFn = void(__thiscall*)(void* self);
	using GetAvailableVoiceFn = int(__thiscall*)(void* self, uint32_t* compressedBytes, uint32_t* uncompressedBytes, uint32_t desiredSampleRate);
	using GetVoiceFn = int(__thiscall*)(void* self, bool wantCompressed,
		void* destination, uint32_t destinationSize, uint32_t* bytesWritten,
		bool wantUncompressed, void* uncompressedDestination,
		uint32_t uncompressedDestinationSize, uint32_t* uncompressedBytesWritten,
		uint32_t desiredSampleRate);
	using DecompressVoiceFn = int(__thiscall*)(void* self, const void* compressed, uint32_t compressedBytes,
		void* destBuffer, uint32_t destBufferSize, uint32_t* bytesWritten, uint32_t desiredSampleRate);

	hook_t* g_startVoiceRecordingHook = nullptr;
	hook_t* g_stopVoiceRecordingHook = nullptr;
	hook_t* g_getAvailableVoiceHook = nullptr;
	hook_t* g_getVoiceHook = nullptr;
	hook_t* g_decompressVoiceHook = nullptr;

	VoiceRecordingControlFn g_originalStartVoiceRecording = nullptr;
	VoiceRecordingControlFn g_originalStopVoiceRecording = nullptr;
	GetAvailableVoiceFn g_originalGetAvailableVoice = nullptr;
	GetVoiceFn g_originalGetVoice = nullptr;
	DecompressVoiceFn g_originalDecompressVoice = nullptr;

	void InstallVoiceSendHook();
	void InstallSteamVoiceHooks();
	void UninstallSteamVoiceHooks();
	void UninstallVoiceSendHook();

	static void (*g_originalHUD_Init)(void) = nullptr;
	static void HookHUD_Init(void)
	{
		DebuLog("HookHUD_Init: map initialization / HUD reset, stopping playback");
		if (!g_voiceSendHook)
		{
			InstallVoiceSendHook();
		}

		if (g_originalHUD_Init)
			g_originalHUD_Init();

		VoiceCore::Get().StopAll();
	}

	void __cdecl HookVoiceSend(int arg0)
	{
		if (!g_originalVoiceSend)
			return;

		if (!g_startVoiceRecordingHook && g_clientStatePtr && *g_clientStatePtr == 5)
		{
			InstallSteamVoiceHooks();
		}

		if (VoiceCore::Get().InjectDueChunks())
			return;

		g_originalVoiceSend(arg0);
	}

	void __fastcall HookStartVoiceRecording(void* self, void*)
	{
		if (!self)
			return;
		DebuLog("HookStartVoiceRecording: physical mic pressed (ducking plugin)");
		if (g_originalStartVoiceRecording)
			g_originalStartVoiceRecording(self);

		VoiceCore::Get().SetUserTalking(true);
	}

	void __fastcall HookStopVoiceRecording(void* self, void*)
	{
		if (!self)
			return;
		DebuLog("HookStopVoiceRecording: physical mic released");
		if (g_originalStopVoiceRecording)
			g_originalStopVoiceRecording(self);

		VoiceCore::Get().SetUserTalking(false);
	}

	int __fastcall HookGetAvailableVoice(void* self, void*,
		uint32_t* compressedBytes, uint32_t* uncompressedBytes, uint32_t desiredSampleRate)
	{
		if (!self)
			return 3; // k_EVoiceResultNoData
		if (VoiceCore::Get().IsTransmitting())
		{
			if (compressedBytes)
				*compressedBytes = 0;
			if (uncompressedBytes)
				*uncompressedBytes = 0;
			return 3; // k_EVoiceResultNoData
		}

		if (!g_originalGetAvailableVoice)
			return 3;

		return g_originalGetAvailableVoice(self, compressedBytes, uncompressedBytes, desiredSampleRate);
	}

	int __fastcall HookGetVoice(void* self, void*, bool wantCompressed,
		void* destination, uint32_t destinationSize, uint32_t* bytesWritten,
		bool wantUncompressed, void* uncompressedDestination,
		uint32_t uncompressedDestinationSize, uint32_t* uncompressedBytesWritten,
		uint32_t desiredSampleRate)
	{
		if (!self)
			return 3; // k_EVoiceResultNoData
		if (VoiceCore::Get().IsTransmitting())
		{
			if (bytesWritten)
				*bytesWritten = 0;
			if (uncompressedBytesWritten)
				*uncompressedBytesWritten = 0;
			return 3; // k_EVoiceResultNoData
		}

		if (!g_originalGetVoice)
			return 3;

		int result = g_originalGetVoice(self, wantCompressed, destination, destinationSize,
			bytesWritten, wantUncompressed, uncompressedDestination,
			uncompressedDestinationSize, uncompressedBytesWritten, desiredSampleRate);

		if (result == 0 && wantCompressed && destination && bytesWritten && *bytesWritten > 0)
		{
			VoiceCore::Get().OnVoicePacketObserved(static_cast<const uint8_t*>(destination), *bytesWritten);
		}

		return result;
	}

	int __fastcall HookDecompressVoice(void* self, void*, const void* compressed,
		uint32_t compressedBytes, void* destBuffer, uint32_t destBufferSize, uint32_t* bytesWritten, uint32_t desiredSampleRate)
	{
		if (!self)
			return 3; // k_EVoiceResultNoData
		if (!g_originalDecompressVoice)
			return 3;

		int result = g_originalDecompressVoice(self, compressed, compressedBytes,
			destBuffer, destBufferSize, bytesWritten, desiredSampleRate);

		if (result == 0 && bytesWritten && *bytesWritten > 0)
		{
			VoiceCore::Get().OnDecompressVoice(*bytesWritten);
		}

		return result;
	}

	void InstallVoiceSendHook()
	{
		if (g_voiceSendHook || !g_pMetaHookAPI)
			return;

		void* engineBase = g_pMetaHookAPI->GetEngineBase();
		DWORD engineSize = g_pMetaHookAPI->GetEngineSize();
		const DWORD sigLen = static_cast<DWORD>(sizeof(kVoiceSendSignature) - 1);

		if (!engineBase || engineSize < sigLen)
			return;

		void* mirrorBase = g_pMetaHookAPI->GetMirrorEngineBase();
		DWORD mirrorSize = g_pMetaHookAPI->GetMirrorEngineSize();
		void* voiceSendAddr = nullptr;

		if (mirrorBase && mirrorSize >= sigLen)
		{
			void* matchInMirror = g_pMetaHookAPI->SearchPattern(mirrorBase, mirrorSize, kVoiceSendSignature, sigLen);
			if (matchInMirror)
			{
				const ULONG_PTR offset = reinterpret_cast<ULONG_PTR>(matchInMirror) - reinterpret_cast<ULONG_PTR>(mirrorBase);
				void* duplicate = nullptr;
				if (offset + sigLen < mirrorSize)
				{
					duplicate = g_pMetaHookAPI->SearchPattern(
						reinterpret_cast<uint8_t*>(mirrorBase) + offset + 1,
						static_cast<DWORD>(mirrorSize - offset - 1),
						kVoiceSendSignature, sigLen);
				}

				if (!duplicate)
				{
					voiceSendAddr = reinterpret_cast<uint8_t*>(engineBase) + offset;
				}
			}
		}

		if (!voiceSendAddr)
		{
			voiceSendAddr = g_pMetaHookAPI->SearchPattern(engineBase, engineSize, kVoiceSendSignature, sigLen);
			if (!voiceSendAddr)
			{
				SafeConPrintf("[VoiceExtension] ERROR: Voice_Send signature not found in engine binary.\n");
				return;
			}

			const ULONG_PTR offset = reinterpret_cast<ULONG_PTR>(voiceSendAddr) - reinterpret_cast<ULONG_PTR>(engineBase);
			void* duplicate = nullptr;
			if (offset + sigLen < engineSize)
			{
				duplicate = g_pMetaHookAPI->SearchPattern(
					reinterpret_cast<uint8_t*>(engineBase) + offset + 1,
					static_cast<DWORD>(engineSize - offset - 1),
					kVoiceSendSignature, sigLen);
			}

			if (duplicate)
			{
				SafeConPrintf("[VoiceExtension] ERROR: Voice_Send signature is ambiguous (multiple matches found).\n");
				return;
			}
		}

		const ULONG_PTR imageBase = reinterpret_cast<ULONG_PTR>(engineBase);

		// The signature is followed by `cmp [clientState], 5`; read that address and keep it inside the module.
		const uint8_t* sigBytes = static_cast<const uint8_t*>(voiceSendAddr);
		const ULONG_PTR stateAddr = *reinterpret_cast<const uint32*>(sigBytes + 22);
		if (sigBytes[20] == 0x83 && sigBytes[21] == 0x3D && stateAddr >= imageBase &&
			stateAddr + sizeof(int) <= imageBase + engineSize)
		{
			g_clientStatePtr = reinterpret_cast<volatile int*>(stateAddr);
		}
		else
		{
			SafeConPrintf("[VoiceExtension] WARNING: Failed to extract clientState pointer from Voice_Send.\n");
		}

		// Dynamically scan Voice_Send instructions to find sizebuf cursor and maxsize globals
		volatile uint32* pMaxSize = nullptr;
		volatile uint32* pCursor = nullptr;
		uint8_t* volatile* pData = nullptr;

		const uint8_t* pCode = sigBytes;
		constexpr size_t kMaxScanBytes = 160;
		for (size_t i = 0; i + 10 < kMaxScanBytes && i + 10 < engineSize; ++i)
		{
			// Look for mov ecx, [disp32] (8B 0D) or mov reg, [disp32] (8B 05 | 0D | 15 | 1D | 25 | 2D | 35 | 3D)
			if (pCode[i] == 0x8B && (pCode[i + 1] & 0xC7) == 0x05)
			{
				uint8_t reg = (pCode[i + 1] >> 3) & 0x07;
				ULONG_PTR candidateCursor = *reinterpret_cast<const uint32*>(pCode + i + 2);

				// Search forward for cmp reg, [disp32] (3B / 39 reg, [disp32])
				for (size_t j = i + 6; j + 6 < kMaxScanBytes && j < i + 32; ++j)
				{
					if ((pCode[j] == 0x3B || pCode[j] == 0x39) && pCode[j + 1] == static_cast<uint8_t>(0x05 | (reg << 3)))
					{
						ULONG_PTR candidateMax = *reinterpret_cast<const uint32*>(pCode + j + 2);
						// In sizebuf_t, maxsize is at offset +4 and cursize is at offset +8
						if (candidateCursor == candidateMax + sizeof(uint32) &&
							candidateMax >= imageBase + sizeof(void*) &&
							candidateCursor + sizeof(uint32) <= imageBase + engineSize)
						{
							pMaxSize = reinterpret_cast<volatile uint32*>(candidateMax);
							pCursor = reinterpret_cast<volatile uint32*>(candidateCursor);
							pData = reinterpret_cast<uint8_t* volatile*>(candidateMax - sizeof(void*));
							break;
						}
					}
				}
				if (pMaxSize)
					break;
			}
		}

		// Fallback to verified RVAs if dynamic scanning did not resolve
		if (!pMaxSize)
		{
			if (engineSize > kVoiceSendCursorRva + sizeof(uint32))
			{
				pMaxSize = reinterpret_cast<volatile uint32*>(reinterpret_cast<uint8_t*>(engineBase) + kVoiceSendMaxSizeRva);
				pCursor = reinterpret_cast<volatile uint32*>(reinterpret_cast<uint8_t*>(engineBase) + kVoiceSendCursorRva);
				pData = reinterpret_cast<uint8_t* volatile*>(reinterpret_cast<uint8_t*>(engineBase) + kVoiceSendDataRva);
			}
		}

		if (!pMaxSize || !pCursor || !pData ||
			reinterpret_cast<ULONG_PTR>(pMaxSize) < imageBase ||
			reinterpret_cast<ULONG_PTR>(pCursor) + sizeof(uint32) > imageBase + engineSize)
		{
			SafeConPrintf("[VoiceExtension] ERROR: Unable to locate sizebuf pointers in engine memory.\n");
			return;
		}

		g_voiceSendData = pData;
		g_voiceSendCursor = pCursor;
		g_voiceSendMaxSize = pMaxSize;

		g_voiceSendHook = g_pMetaHookAPI->InlineHook(voiceSendAddr,
			reinterpret_cast<void*>(&HookVoiceSend), reinterpret_cast<void**>(&g_originalVoiceSend));

		if (g_voiceSendHook)
		{
			DebuLog("InstallVoiceSendHook: SUCCESS (maxsize=%u, cursor=%p, clientStatePtr=%p)",
				*pMaxSize, static_cast<const void*>(const_cast<uint32*>(pCursor)), static_cast<void*>(const_cast<int*>(g_clientStatePtr)));
			SafeConPrintf("[VoiceExtension] Voice_Send hook installed successfully (maxsize=%u, clientStatePtr=%p).\n",
				*pMaxSize, static_cast<void*>(const_cast<int*>(g_clientStatePtr)));
		}
		else
		{
			DebuLog("InstallVoiceSendHook: ERROR failed to install inline hook");
			SafeConPrintf("[VoiceExtension] ERROR: Failed to install InlineHook on Voice_Send.\n");
		}
	}

	void UninstallVoiceSendHook()
	{
		DebuLog("UninstallVoiceSendHook invoked");
		if (g_voiceSendHook && g_pMetaHookAPI)
		{
			g_pMetaHookAPI->UnHook(g_voiceSendHook);
			g_voiceSendHook = nullptr;
			g_originalVoiceSend = nullptr;
		}
		g_voiceSendData = nullptr;
		g_voiceSendCursor = nullptr;
		g_voiceSendMaxSize = nullptr;
		g_clientStatePtr = nullptr;
	}

	void InstallSteamVoiceHooks()
	{
		if (g_startVoiceRecordingHook || g_getVoiceHook || !g_pMetaHookAPI)
			return;

		HMODULE hSteamApi = GetModuleHandleA("steam_api.dll");
		if (!hSteamApi)
			return;

		using FnSteamAPI_IsSteamRunning = bool(*)();
		using FnGetHSteamUser = int32_t(*)();
		using FnFindOrCreateUserInterface = void*(*)(int32_t, const char*);

		auto pfnIsSteamRunning = reinterpret_cast<FnSteamAPI_IsSteamRunning>(GetProcAddress(hSteamApi, "SteamAPI_IsSteamRunning"));
		if (pfnIsSteamRunning && !pfnIsSteamRunning())
		{
			static bool s_warnedNotRunning = false;
			if (!s_warnedNotRunning)
			{
				s_warnedNotRunning = true;
				SafeConPrintf("[VoiceExtension] SteamAPI_IsSteamRunning returned false; deferring SteamUser referee hooks.\n");
			}
			return;
		}

		auto pfnGetHSteamUser = reinterpret_cast<FnGetHSteamUser>(GetProcAddress(hSteamApi, "SteamAPI_GetHSteamUser"));
		auto pfnFindOrCreateUserInterface = reinterpret_cast<FnFindOrCreateUserInterface>(GetProcAddress(hSteamApi, "SteamInternal_FindOrCreateUserInterface"));

		if (!pfnGetHSteamUser || !pfnFindOrCreateUserInterface)
			return;

		int32_t hUser = pfnGetHSteamUser();
		if (!hUser)
			return;

		void* pSteamUser = pfnFindOrCreateUserInterface(hUser, "SteamUser020");
		if (!pSteamUser)
			return;

		void** vtable = *reinterpret_cast<void***>(pSteamUser);
		if (!vtable || !g_pMetaHookAPI->VFTHookEx)
		{
			DebuLog("InstallSteamVoiceHooks: invalid vtable or VFTHookEx");
			return;
		}
		g_startVoiceRecordingHook = g_pMetaHookAPI->VFTHookEx(vtable, 7,
			reinterpret_cast<void*>(&HookStartVoiceRecording),
			reinterpret_cast<void**>(&g_originalStartVoiceRecording));

		g_stopVoiceRecordingHook = g_pMetaHookAPI->VFTHookEx(vtable, 8,
			reinterpret_cast<void*>(&HookStopVoiceRecording),
			reinterpret_cast<void**>(&g_originalStopVoiceRecording));

		g_getAvailableVoiceHook = g_pMetaHookAPI->VFTHookEx(vtable, 9,
			reinterpret_cast<void*>(&HookGetAvailableVoice),
			reinterpret_cast<void**>(&g_originalGetAvailableVoice));

		g_getVoiceHook = g_pMetaHookAPI->VFTHookEx(vtable, 10,
			reinterpret_cast<void*>(&HookGetVoice),
			reinterpret_cast<void**>(&g_originalGetVoice));

		g_decompressVoiceHook = g_pMetaHookAPI->VFTHookEx(vtable, 11,
			reinterpret_cast<void*>(&HookDecompressVoice),
			reinterpret_cast<void**>(&g_originalDecompressVoice));

		if (!g_startVoiceRecordingHook || !g_stopVoiceRecordingHook || !g_getAvailableVoiceHook || !g_getVoiceHook || !g_decompressVoiceHook)
		{
			UninstallSteamVoiceHooks();
			DebuLog("InstallSteamVoiceHooks: ERROR failed to install all referee hooks");
			SafeConPrintf("[VoiceExtension] ERROR: Failed to install all SteamUser020 referee hooks.\n");
			return;
		}

		DebuLog("InstallSteamVoiceHooks: SUCCESS referee hooks installed on SteamUser020");
		SafeConPrintf("[VoiceExtension] SteamUser020 referee hooks installed (Start/Stop/GetVoice/Decompress).\n");
	}

	void UninstallSteamVoiceHooks()
	{
		DebuLog("UninstallSteamVoiceHooks invoked");
		if (g_startVoiceRecordingHook && g_pMetaHookAPI)
		{
			g_pMetaHookAPI->UnHook(g_startVoiceRecordingHook);
			g_startVoiceRecordingHook = nullptr;
		}
		if (g_stopVoiceRecordingHook && g_pMetaHookAPI)
		{
			g_pMetaHookAPI->UnHook(g_stopVoiceRecordingHook);
			g_stopVoiceRecordingHook = nullptr;
		}
		if (g_getAvailableVoiceHook && g_pMetaHookAPI)
		{
			g_pMetaHookAPI->UnHook(g_getAvailableVoiceHook);
			g_getAvailableVoiceHook = nullptr;
		}
		if (g_getVoiceHook && g_pMetaHookAPI)
		{
			g_pMetaHookAPI->UnHook(g_getVoiceHook);
			g_getVoiceHook = nullptr;
		}
		if (g_decompressVoiceHook && g_pMetaHookAPI)
		{
			g_pMetaHookAPI->UnHook(g_decompressVoiceHook);
			g_decompressVoiceHook = nullptr;
		}
	}

	// Console Commands
	void Cmd_Play()
	{
		if (!g_ve_allow_server || g_ve_allow_server->value <= 0.0f)
		{
			SafeConPrintf("[VoiceExtension] Console playback ignored (ve_allow_server is 0).\n");
			return;
		}

		if (gEngfuncs.Cmd_Argc() < 2)
		{
			SafeConPrintf("Usage: ve_play <sound_path> [volume] [pitch] [priority]\n");
			return;
		}

		const char* soundPath = gEngfuncs.Cmd_Argv(1);
		DebuLog("Cmd_Play: input path='%s'", soundPath ? soundPath : "(null)");
		if (!VoiceCore::IsPathSafe(soundPath))
		{
			DebuLog("Cmd_Play: path rejected as unsafe '%s'", soundPath);
			SafeConPrintf("[VoiceExtension] Rejected path (unsafe characters or outside sound dirs): '%s'\n", soundPath);
			return;
		}

		VoicePlayParams params;
		if (gEngfuncs.Cmd_Argc() >= 3)
			params.volume = static_cast<float>(atof(gEngfuncs.Cmd_Argv(2)));
		if (gEngfuncs.Cmd_Argc() >= 4)
			params.pitch = static_cast<float>(atof(gEngfuncs.Cmd_Argv(3)));
		if (gEngfuncs.Cmd_Argc() >= 5)
			params.priority = static_cast<VoicePriority>(atoi(gEngfuncs.Cmd_Argv(4)));

		DebuLog("Cmd_Play: enqueuing '%s' vol=%.2f pitch=%.2f prio=%d", soundPath, params.volume, params.pitch, static_cast<int>(params.priority));
		uint32_t trackId = VoiceCore::Get().EnqueueFile(soundPath, params);
		if (trackId > 0)
		{
			DebuLog("Cmd_Play: Enqueued track #%u successfully", trackId);
			SafeConPrintf("[VoiceExtension] Enqueued track #%u: '%s'\n", trackId, soundPath);
		}
		else
		{
			DebuLog("Cmd_Play: Failed to enqueue track '%s'", soundPath);
			SafeConPrintf("[VoiceExtension] Failed to play: '%s' (not found or invalid format)\n", soundPath);
		}
	}

	void Cmd_ServerPlay()
	{
		Cmd_Play();
	}

	void Cmd_Stop()
	{
		// Always allowed as an emergency kill-switch (analogous to engine stopsound)
		DebuLog("Cmd_Stop invoked");
		VoiceCore::Get().StopAll();
		SafeConPrintf("[VoiceExtension] Playback stopped and queue cleared.\n");
	}

	void Cmd_Pause()
	{
		if (!g_ve_allow_server || g_ve_allow_server->value <= 0.0f)
		{
			SafeConPrintf("[VoiceExtension] Console command ignored (ve_allow_server is 0).\n");
			return;
		}
		DebuLog("Cmd_Pause invoked");
		VoiceCore::Get().Pause(true);
		SafeConPrintf("[VoiceExtension] Playback paused.\n");
	}

	void Cmd_Resume()
	{
		if (!g_ve_allow_server || g_ve_allow_server->value <= 0.0f)
		{
			SafeConPrintf("[VoiceExtension] Console command ignored (ve_allow_server is 0).\n");
			return;
		}
		DebuLog("Cmd_Resume invoked");
		VoiceCore::Get().Pause(false);
		SafeConPrintf("[VoiceExtension] Playback resumed.\n");
	}

	void Cmd_Panic()
	{
		if (!g_ve_allow_server || g_ve_allow_server->value <= 0.0f)
		{
			SafeConPrintf("[VoiceExtension] Console command ignored (ve_allow_server is 0).\n");
			return;
		}
		const char* reason = (gEngfuncs.Cmd_Argc && gEngfuncs.Cmd_Argc() > 1 && gEngfuncs.Cmd_Argv)
			? gEngfuncs.Cmd_Argv(1) : "manual";
		VoiceCore::Get().Panic(reason);
		SafeConPrintf("[VoiceExtension] PANIC engaged (reason: %s). Use ve_unpanic to exit.\n", reason);
	}

	void Cmd_Unpanic()
	{
		if (!VoiceCore::Get().IsPanic())
		{
			SafeConPrintf("[VoiceExtension] Not in panic mode.\n");
			return;
		}
		VoiceCore::Get().Unpanic();
		SafeConPrintf("[VoiceExtension] Panic cleared. Ready.\n");
	}

	void Cmd_Status()
	{
		VoicePlaybackState state = VoiceCore::Get().GetState();
		const char* stateStr = "IDLE";
		switch (state)
		{
		case VoicePlaybackState::PREROLL:  stateStr = "PREROLL"; break;
		case VoicePlaybackState::PLAYING:  stateStr = "PLAYING"; break;
		case VoicePlaybackState::DUCKED:   stateStr = "PAUSED_BY_USER_MIC"; break;
		case VoicePlaybackState::POSTROLL: stateStr = "POSTROLL"; break;
		default: break;
		}
		const bool bPanic = VoiceCore::Get().IsPanic();
		if (bPanic) stateStr = "PANIC";

		uint64_t steamId = VoiceCore::Get().GetCurrentSteamID();
		const auto& tel = VoiceCore::Get().GetTelemetry();

		SafeConPrintf("=== VoiceExtension Telemetry Report (Build %s %s) ===\n", __DATE__, __TIME__);
		SafeConPrintf("HOOKS: SteamUser020=%s, Voice_Send=%s\n",
			g_getVoiceHook ? "OK" : "NO", g_voiceSendHook ? "OK" : "NO");
		SafeConPrintf("STATUS: state=%s, transmitting=%s, user_mic=%s, queue=%u tracks\n",
			stateStr,
			VoiceCore::Get().IsTransmitting() ? "YES" : "NO",
			VoiceCore::Get().IsUserTalking() ? "ACTIVE" : "SILENT",
			static_cast<unsigned>(VoiceCore::Get().GetQueueSize()));
		if (bPanic)
		{
			char panicBuf[256] = {};
			VoiceCore::Get().GetPanicReason(panicBuf, sizeof(panicBuf));
			SafeConPrintf("*** PANIC *** reason: %s\n", panicBuf);
		}
		SafeConPrintf("STEAMID64: %llu (%s)\n", steamId, steamId != 0 ? "VALID" : "UNRESOLVED / 0");
		SafeConPrintf("ENGINE BUFFER: cursor=%u, maxsize=%u (expected %u), client_state=%d\n",
			g_voiceSendCursor ? *g_voiceSendCursor : 0,
			g_voiceSendMaxSize ? *g_voiceSendMaxSize : 0,
			kVoiceSendExpectedMaxSize,
			g_clientStatePtr ? *g_clientStatePtr : -1);
		SafeConPrintf("INJECTION: sent=%llu frames in %llu blocks, unflushed_frames=%llu\n",
			tel.injectedFrames, tel.injectedPackets, tel.unflushedFrames);
		SafeConPrintf("DROPS: steamid_zero=%llu, state_not_5=%llu, buffer_full=%llu\n",
			tel.droppedSteamIdZero, tel.droppedStateNot5, tel.droppedBufferFull);
		SafeConPrintf("RECEIVE: %llu pkts (%llu B), jitter_events=%u, max_gap=%.1f ms\n",
			tel.rxPackets, tel.rxBytes, tel.rxJitterCount, tel.maxRxGapUs / 1000.0);
		SafeConPrintf("===============================================================\n");
	}
}

// -----------------------------------------------------------------------------
// CVoiceExtension Interface Implementation
// -----------------------------------------------------------------------------
CVoiceExtension g_VoiceExtension;

uint32_t CVoiceExtension::EnqueueFile(const char* relativePath, const VoicePlayParams& params)
{
	return VoiceCore::Get().EnqueueFile(relativePath, params, false);
}

uint32_t CVoiceExtension::EnqueuePcm(const int16_t* pcm, size_t samples, int sampleRate, const VoicePlayParams& params)
{
	return VoiceCore::Get().EnqueuePcm(pcm, samples, sampleRate, params);
}

void CVoiceExtension::StopAll()
{
	VoiceCore::Get().StopAll();
}

void CVoiceExtension::StopCurrent()
{
	VoiceCore::Get().StopCurrent();
}

void CVoiceExtension::ClearQueue()
{
	VoiceCore::Get().ClearQueue();
}

void CVoiceExtension::Pause(bool pause)
{
	VoiceCore::Get().Pause(pause);
}

bool CVoiceExtension::OpenStream(int sampleRate, const VoicePlayParams& params)
{
	return VoiceCore::Get().OpenStream(sampleRate, params);
}

bool CVoiceExtension::PushStreamPcm(const int16_t* pcm, size_t samples)
{
	return VoiceCore::Get().PushStreamPcm(pcm, samples);
}

void CVoiceExtension::CloseStream()
{
	VoiceCore::Get().CloseStream();
}

VoicePlaybackState CVoiceExtension::GetState() const
{
	return VoiceCore::Get().GetState();
}

bool CVoiceExtension::IsTransmitting() const
{
	return VoiceCore::Get().IsTransmitting();
}

size_t CVoiceExtension::GetQueueSize() const
{
	return VoiceCore::Get().GetQueueSize();
}

uint64_t CVoiceExtension::GetCurrentSteamID() const
{
	return VoiceCore::Get().GetCurrentSteamID();
}

void CVoiceExtension::RegisterListener(IVoiceEventListener* listener)
{
	VoiceCore::Get().RegisterListener(listener);
}

void CVoiceExtension::UnregisterListener(IVoiceEventListener* listener)
{
	VoiceCore::Get().UnregisterListener(listener);
}

bool CVoiceExtension::IsPanic() const
{
	return VoiceCore::Get().IsPanic();
}

const char* CVoiceExtension::GetPanicReason() const
{
	return VoiceCore::Get().IsPanic() ? VoiceCore::Get().PanicReason() : "";
}

bool CVoiceExtension::IsReady() const
{
	if (!g_voiceSendHook)
	{
		InstallVoiceSendHook();
	}
	return (g_voiceSendHook != nullptr && g_clientStatePtr != nullptr && !VoiceCore::Get().IsPanic());
}

uint32_t CVoiceExtension::RegisterClient(const char* clientName, const VoiceClientParams& params)
{
	return VoiceCore::Get().RegisterClient(clientName, params);
}

void CVoiceExtension::UnregisterClient(uint32_t clientId)
{
	VoiceCore::Get().UnregisterClient(clientId);
}

uint32_t CVoiceExtension::EnqueueFileEx(uint32_t clientId, const char* relativePath, const VoicePlayParams& params)
{
	return VoiceCore::Get().EnqueueFile(relativePath, params, false, clientId);
}

uint32_t CVoiceExtension::EnqueuePcmEx(uint32_t clientId, const int16_t* pcm, size_t samples, int sampleRate, const VoicePlayParams& params)
{
	return VoiceCore::Get().EnqueuePcm(pcm, samples, sampleRate, params, clientId);
}

bool CVoiceExtension::OpenStreamEx(uint32_t clientId, int sampleRate, const VoicePlayParams& params)
{
	return VoiceCore::Get().OpenStream(sampleRate, params, clientId);
}

bool CVoiceExtension::PushStreamPcmEx(uint32_t clientId, const int16_t* pcm, size_t samples)
{
	return VoiceCore::Get().PushStreamPcm(pcm, samples, clientId);
}

void CVoiceExtension::CloseStreamEx(uint32_t clientId)
{
	VoiceCore::Get().CloseStream(clientId);
}

bool CVoiceExtension::IsStreamingActiveEx(uint32_t clientId) const
{
	return VoiceCore::Get().IsStreamingActive(clientId);
}

uint32_t CVoiceExtension::GetStreamOwner() const
{
	return VoiceCore::Get().GetStreamOwner();
}

bool CVoiceExtension::StopTrack(uint32_t trackId)
{
	return VoiceCore::Get().StopTrack(trackId);
}

void CVoiceExtension::StopClient(uint32_t clientId)
{
	VoiceCore::Get().StopClient(clientId);
}

size_t CVoiceExtension::GetClientQueueSize(uint32_t clientId) const
{
	return VoiceCore::Get().GetClientQueueSize(clientId);
}

uint32_t CVoiceExtension::GetTrackOwner(uint32_t trackId) const
{
	return VoiceCore::Get().GetTrackOwner(trackId);
}

EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CVoiceExtension, IVoiceExtension, VOICE_EXTENSION_INTERFACE_VERSION, g_VoiceExtension);
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CVoiceExtension, IVoiceExtension2, VOICE_EXTENSION_INTERFACE_VERSION_2, g_VoiceExtension);
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CVoiceExtension, IVoiceExtension3, VOICE_EXTENSION_INTERFACE_VERSION_3, g_VoiceExtension);

// -----------------------------------------------------------------------------
// Plugin Lifecycle (IPluginsV4)
// -----------------------------------------------------------------------------
class CVoiceExtensionPlugin final : public IPluginsV4
{
public:
	void Init(metahook_api_t* api, mh_interface_t* pInterface, mh_enginesave_t* pSave) override
	{
		DebuLog("CVoiceExtensionPlugin::Init: api=%p, pInterface=%p, pSave=%p", api, pInterface, pSave);
		g_pMetaHookAPI = api;
		g_pInterface = pInterface;
		g_pMetaSave = pSave;

		VoiceCore::Get().Init();
		VoicePipe_StartServer();
	}

	void Shutdown() override
	{
		DebuLog("CVoiceExtensionPlugin::Shutdown: stopping servers and uninstalling hooks");
		VoicePipe_StopServer();
		UninstallSteamVoiceHooks();
		UninstallVoiceSendHook();
		VoiceCore::Get().Shutdown();
	}

	void LoadEngine(cl_enginefunc_t* engineFuncs) override
	{
		DebuLog("CVoiceExtensionPlugin::LoadEngine: engineFuncs=%p", engineFuncs);
		if (engineFuncs)
			memcpy(&gEngfuncs, engineFuncs, sizeof(gEngfuncs));

		InstallVoiceSendHook();
	}

	void LoadClient(cl_exportfuncs_t* exportFuncs) override
	{
		DebuLog("CVoiceExtensionPlugin::LoadClient: exportFuncs=%p", exportFuncs);
		if (exportFuncs)
		{
			memcpy(&gExportfuncs, exportFuncs, sizeof(gExportfuncs));
			if (exportFuncs->HUD_Init && exportFuncs->HUD_Init != HookHUD_Init)
			{
				g_originalHUD_Init = exportFuncs->HUD_Init;
				exportFuncs->HUD_Init = HookHUD_Init;
			}
		}

		if (!g_voiceSendHook)
		{
			InstallVoiceSendHook();
		}

		InstallSteamVoiceHooks();

		static bool bCommandsRegistered = false;
		if (!bCommandsRegistered)
		{
			bCommandsRegistered = true;
			if (gEngfuncs.pfnAddCommand)
			{
				gEngfuncs.pfnAddCommand("ve_play", Cmd_Play);
				gEngfuncs.pfnAddCommand("ve_serverplay", Cmd_ServerPlay);
				gEngfuncs.pfnAddCommand("ve_stop", Cmd_Stop);
				gEngfuncs.pfnAddCommand("ve_pause", Cmd_Pause);
				gEngfuncs.pfnAddCommand("ve_resume", Cmd_Resume);
				gEngfuncs.pfnAddCommand("ve_status", Cmd_Status);
				gEngfuncs.pfnAddCommand("ve_panic", Cmd_Panic);
				gEngfuncs.pfnAddCommand("ve_unpanic", Cmd_Unpanic);
			}
			// Register configurable cvars.
			// pfnRegisterVariable(name, defaultValue, flags) returns the live cvar_t*.
			// Security rule: ve_allow_abs_paths and ve_allow_server are NEVER FCVAR_ARCHIVE.
			if (gEngfuncs.pfnRegisterVariable)
			{
				g_ve_duck_resume_ms = gEngfuncs.pfnRegisterVariable("ve_duck_resume_ms", "150", FCVAR_ARCHIVE);
				g_ve_allow_abs_paths = gEngfuncs.pfnRegisterVariable("ve_allow_abs_paths", "0", 0);
				g_ve_allow_server = gEngfuncs.pfnRegisterVariable("ve_allow_server", "0", 0);
				g_ve_debug = gEngfuncs.pfnRegisterVariable("ve_debug", "0", 0);
			}
		}
	}

	void ExitGame(int iResult) override
	{
		VoiceCore::Get().StopAll();
	}

	const char* GetVersion(void) override
	{
		return "1.0.0";
	}
};

const char* IPluginsV4::GetVersion(void)
{
	return "1.0.0";
}

EXPOSE_SINGLE_INTERFACE(CVoiceExtensionPlugin, IPluginsV4, METAHOOK_PLUGIN_API_VERSION_V4);

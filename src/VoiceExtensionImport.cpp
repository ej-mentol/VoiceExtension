#include <metahook.h>
#include "VoiceExtensionImport.h"

static IVoiceExtension* g_pVoiceExtension = nullptr;
static IVoiceExtension2* g_pVoiceExtension2 = nullptr;
static IVoiceExtension3* g_pVoiceExtension3 = nullptr;

static HMODULE FindVE()
{
	HMODULE h = GetModuleHandleA("VoiceExtension.dll");
	return h ? h : GetModuleHandleA("VoiceExtension_AVX2.dll");
}

void VoiceExtension_Init()
{
	HMODULE hModule = FindVE();
	if (!hModule)
		return;

	auto factory = Sys_GetFactory((HINTERFACEMODULE)hModule);
	if (!factory)
		return;

	g_pVoiceExtension3 = (IVoiceExtension3*)factory(VOICE_EXTENSION_INTERFACE_VERSION_3, NULL);
	g_pVoiceExtension2 = g_pVoiceExtension3 ? g_pVoiceExtension3 : (IVoiceExtension2*)factory(VOICE_EXTENSION_INTERFACE_VERSION_2, NULL);
	g_pVoiceExtension = g_pVoiceExtension2 ? g_pVoiceExtension2 : (IVoiceExtension*)factory(VOICE_EXTENSION_INTERFACE_VERSION, NULL);
}

void VoiceExtension_Shutdown()
{
	g_pVoiceExtension3 = nullptr;
	g_pVoiceExtension2 = nullptr;
	g_pVoiceExtension = nullptr;
}

IVoiceExtension* VoiceExtension()
{
	HMODULE hModule = FindVE();
	if (!hModule)
	{
		g_pVoiceExtension = nullptr;
		g_pVoiceExtension2 = nullptr;
		g_pVoiceExtension3 = nullptr;
		return nullptr;
	}

	if (!g_pVoiceExtension)
	{
		VoiceExtension_Init();
	}

	return g_pVoiceExtension;
}

IVoiceExtension2* VoiceExtension2()
{
	HMODULE hModule = FindVE();
	if (!hModule)
	{
		g_pVoiceExtension = nullptr;
		g_pVoiceExtension2 = nullptr;
		g_pVoiceExtension3 = nullptr;
		return nullptr;
	}

	if (!g_pVoiceExtension2)
	{
		VoiceExtension_Init();
	}

	return g_pVoiceExtension2;
}

IVoiceExtension3* VoiceExtension3()
{
	HMODULE hModule = FindVE();
	if (!hModule)
	{
		g_pVoiceExtension = nullptr;
		g_pVoiceExtension2 = nullptr;
		g_pVoiceExtension3 = nullptr;
		return nullptr;
	}

	if (!g_pVoiceExtension3)
	{
		VoiceExtension_Init();
	}

	return g_pVoiceExtension3;
}

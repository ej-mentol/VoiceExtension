#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef DEBULOG_TAG
#define DEBULOG_TAG "VE"
#endif

#include <cvardef.h>

extern cvar_t* g_ve_debug;

// One-line synchronous diagnostic logger for DebugView / kernel monitor.
// Outputs immediately via OutputDebugStringA with tag, thread ID, and newline formatting only if debug is enabled.
inline void DebuLogImpl(const char* tag, const char* fmt, ...)
{
	// Zero CPU overhead when debug is disabled (default)
	if (!g_ve_debug || g_ve_debug->value <= 0.0f)
		return;
	char msg[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);
	msg[sizeof(msg) - 1] = '\0';

	char fullMsg[1152];
	DWORD tid = GetCurrentThreadId();
	snprintf(fullMsg, sizeof(fullMsg), "[%s][TID:%lu] %s", tag ? tag : "MH", static_cast<unsigned long>(tid), msg);
	fullMsg[sizeof(fullMsg) - 1] = '\0';

	size_t len = strlen(fullMsg);
	if (len > 0 && fullMsg[len - 1] != '\n' && len + 1 < sizeof(fullMsg))
	{
		fullMsg[len] = '\n';
		fullMsg[len + 1] = '\0';
	}

	OutputDebugStringA(fullMsg);
}

#define DebuLog(...) DebuLogImpl(DEBULOG_TAG, __VA_ARGS__)
#define DebugLog(...) DebuLogImpl(DEBULOG_TAG, __VA_ARGS__)

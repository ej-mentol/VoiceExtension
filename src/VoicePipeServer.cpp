#include "VoicePipeServer.h"
#include "VoiceCore.h"
#include "DebugLog.h"
#include <windows.h>
#include <process.h>
#include <atomic>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

namespace
{
	constexpr uint32_t kMaxMessageBytes = 16 * 1024 * 1024;
	constexpr size_t   kReadChunkBytes  = 64 * 1024;
	constexpr int      kMaxClients      = 8;

	std::atomic<bool>     g_bPipeRunning{ false };
	std::atomic<int>      g_activeClients{ 0 };
	HANDLE g_hAcceptThread = nullptr;
	HANDLE g_hStopEvent = nullptr;

	struct ClientContext
	{
		HANDLE   hPipe;
		uint32_t id;
	};

	float SanitizeFloat(float v, float lo, float hi, float fallback)
	{
		return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
	}

	std::string ReadToken(std::istringstream& iss)
	{
		iss >> std::ws;
		if (iss.peek() == '"')
		{
			iss.get();
			std::string token;
			std::getline(iss, token, '"');
			return token;
		}
		std::string token;
		iss >> token;
		return token;
	}

	void ProcessTextMessage(const std::string& line, uint32_t clientId)
	{
		if (line.empty())
			return;

		std::istringstream iss(line);
		std::string cmd = ReadToken(iss);
		for (char& c : cmd)
			c = static_cast<char>(toupper(static_cast<unsigned char>(c)));

		if (cmd == "PLAY")
		{
			const std::string path = ReadToken(iss);
			if (path.empty() || !VoiceCore::IsPathSafe(path.c_str(), true))
				return;

			VoicePlayParams params;
			std::string arg;
			if (!(arg = ReadToken(iss)).empty()) params.volume = SanitizeFloat(static_cast<float>(atof(arg.c_str())), 0.0f, 2.0f, 1.0f);
			if (!(arg = ReadToken(iss)).empty()) params.pitch = SanitizeFloat(static_cast<float>(atof(arg.c_str())), 0.5f, 2.0f, 1.0f);
			if (!(arg = ReadToken(iss)).empty()) params.priority = static_cast<VoicePriority>(std::clamp(atoi(arg.c_str()), 0, 3));
			VoiceCore::Get().EnqueueFile(path.c_str(), params, true, clientId);
		}
		else if (cmd == "STOP")
		{
			VoiceCore::Get().StopClient(clientId);
		}
		else if (cmd == "PAUSE")
		{
			if (VoiceCore::Get().IsCurrentTrackOwner(clientId))
			{
				const std::string arg = ReadToken(iss);
				VoiceCore::Get().Pause(arg.empty() ? true : atoi(arg.c_str()) != 0);
			}
		}
		else if (cmd == "RESUME")
		{
			if (VoiceCore::Get().IsCurrentTrackOwner(clientId))
			{
				VoiceCore::Get().Pause(false);
			}
		}
		else if (cmd == "CLEAR")
		{
			VoiceCore::Get().ClearClientQueue(clientId);
		}
		else if (cmd == "STREAM_END" || cmd == "CLOSE_STREAM" || cmd == "CLOSE" || cmd == "STREAM_CLOSE")
		{
			VoiceCore::Get().CloseStream(clientId);
		}
	}

	// Returns an empty vector for unsupported channel counts or truncated input.
	std::vector<int16_t> ToMono(const uint8_t* payload, size_t payloadLen, uint8_t channels)
	{
		std::vector<int16_t> mono;
		if (channels == 1)
		{
			mono.resize(payloadLen / sizeof(int16_t));
			memcpy(mono.data(), payload, mono.size() * sizeof(int16_t));
		}
		else if (channels == 2)
		{
			const size_t frames = payloadLen / (2 * sizeof(int16_t));
			mono.resize(frames);
			for (size_t i = 0; i < frames; ++i)
			{
				int16_t l, r;
				memcpy(&l, payload + i * 4, 2);
				memcpy(&r, payload + i * 4 + 2, 2);
				mono[i] = static_cast<int16_t>((static_cast<int32_t>(l) + r) / 2);
			}
		}
		return mono;
	}

	void HandleBinary(const VoicePipeHeader& hdr, const uint8_t* payload, size_t available, uint32_t clientId)
	{
		// Spec says dataBytes is the exact payload size. A mismatch means a broken sender: drop it
		// instead of silently playing a truncated clip.
		if (hdr.dataBytes != available)
			return;

		VoicePlayParams params;
		params.volume   = SanitizeFloat(hdr.volume, 0.0f, 2.0f, 1.0f);
		params.pitch    = SanitizeFloat(hdr.pitch, 0.5f, 2.0f, 1.0f);
		params.priority = static_cast<VoicePriority>(std::clamp(static_cast<int>(hdr.priority), 0, 3));

		const bool rateOk = hdr.sampleRate >= 8000 && hdr.sampleRate <= 48000;
		VoiceCore& core = VoiceCore::Get();

		switch (hdr.command)
		{
		case PIPE_CMD_PLAY_FILE:
		{
			std::string path(reinterpret_cast<const char*>(payload), available);
			while (!path.empty() && (path.back() == '\0' || path.back() == '\r' || path.back() == '\n'))
				path.pop_back();
			if (!path.empty() && VoiceCore::IsPathSafe(path.c_str(), true))
				core.EnqueueFile(path.c_str(), params, true, clientId);
			break;
		}
		case PIPE_CMD_PLAY_PCM:
		{
			if (!rateOk) break;
			std::vector<int16_t> mono = ToMono(payload, available, hdr.channels);
			if (!mono.empty())
				core.EnqueuePcm(mono.data(), mono.size(), hdr.sampleRate, params, clientId);
			break;
		}
		case PIPE_CMD_STREAM_CHUNK:
		{
			if (!rateOk) break;

			if (!core.IsStreamingActive(clientId) && !core.OpenStream(hdr.sampleRate, params, clientId))
			{
				break;
			}

			std::vector<int16_t> mono = ToMono(payload, available, hdr.channels);
			if (!mono.empty())
				core.PushStreamPcm(mono.data(), mono.size(), clientId);
			break;
		}
		case PIPE_CMD_STREAM_END:
			core.CloseStream(clientId);
			break;
		case PIPE_CMD_STOP:
			core.StopClient(clientId);
			break;
		case PIPE_CMD_PAUSE:
			if (core.IsCurrentTrackOwner(clientId))
				core.Pause(true);
			break;
		case PIPE_CMD_RESUME:
			if (core.IsCurrentTrackOwner(clientId))
				core.Pause(false);
			break;
		default:
			break;
		}
	}

	void HandleMessage(const std::vector<uint8_t>& msg, uint32_t clientId)
	{
		if (msg.size() >= sizeof(VoicePipeHeader))
		{
			VoicePipeHeader hdr;
			memcpy(&hdr, msg.data(), sizeof(hdr));
			if (hdr.magic == VOICE_PIPE_MAGIC)
			{
				HandleBinary(hdr, msg.data() + sizeof(hdr), msg.size() - sizeof(hdr), clientId);
				return;
			}
		}
		ProcessTextMessage(std::string(reinterpret_cast<const char*>(msg.data()), msg.size()), clientId);
	}

	enum class ReadResult { Message, Closed, Stopped };

	// Reads ONE complete pipe message (PIPE_READMODE_MESSAGE), reassembling it from
	// ERROR_MORE_DATA fragments.
	ReadResult ReadMessage(HANDLE hPipe, OVERLAPPED& ov, std::vector<uint8_t>& scratch, std::vector<uint8_t>& message)
	{
		message.clear();
		if (!hPipe || !ov.hEvent)
			return ReadResult::Closed;

		for (;;)
		{
			ResetEvent(ov.hEvent);
			DWORD bytes = 0;
			BOOL ok = ReadFile(hPipe, scratch.data(), static_cast<DWORD>(scratch.size()), nullptr, &ov);
			DWORD err = ok ? ERROR_SUCCESS : GetLastError();

			if (!ok && err == ERROR_IO_PENDING)
			{
				HANDLE waits[2] = { g_hStopEvent, ov.hEvent };
				if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0)
				{
					CancelIoEx(hPipe, &ov);
					GetOverlappedResult(hPipe, &ov, &bytes, TRUE); // Wait for cancellation cleanup
					return ReadResult::Stopped;
				}
				ok = GetOverlappedResult(hPipe, &ov, &bytes, FALSE);
				err = ok ? ERROR_SUCCESS : GetLastError();
			}
			else if (ok || err == ERROR_MORE_DATA)
			{
				BOOL gorOk = GetOverlappedResult(hPipe, &ov, &bytes, FALSE);
				if (!gorOk)
				{
					err = GetLastError();
					if (err != ERROR_MORE_DATA)
						return ReadResult::Closed;
					ok = FALSE;
				}
				else
				{
					ok = TRUE;
					err = ERROR_SUCCESS;
				}
			}
			else
			{
				return ReadResult::Closed;
			}

			if (!ok && err != ERROR_MORE_DATA)
				return ReadResult::Closed;

			message.insert(message.end(), scratch.begin(), scratch.begin() + bytes);
			if (message.size() > kMaxMessageBytes)
				return ReadResult::Closed; // protocol violation, drop client

			if (ok)
				return ReadResult::Message; // complete message
		}
	}

	unsigned __stdcall ClientThread(void* param)
	{
		std::unique_ptr<ClientContext> ctx(static_cast<ClientContext*>(param));

		OVERLAPPED ov = {};
		ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (!ov.hEvent)
		{
			VoiceCore::Get().UnregisterClient(ctx->id, false);
			DisconnectNamedPipe(ctx->hPipe);
			CloseHandle(ctx->hPipe);
			g_activeClients.fetch_sub(1);
			return 0;
		}
		std::vector<uint8_t> scratch(kReadChunkBytes);
		std::vector<uint8_t> message;

		// Vector growth / message reassembly can throw bad_alloc on a 32-bit process;
		// never let that escape a worker thread (it terminates the game process).
		try
		{
			while (g_bPipeRunning)
			{
				if (ReadMessage(ctx->hPipe, ov, scratch, message) != ReadResult::Message)
					break;
				if (message.empty())
					continue;

				try
				{
					HandleMessage(message, ctx->id);
				}
				catch (...)
				{
					// Prevent any exception from escaping into the game engine
				}
			}
		}
		catch (...)
		{
			DebuLog("ClientThread: exception while reading/handling pipe message");
		}

		VoiceCore::Get().UnregisterClient(ctx->id, false); // Detach client and close dangling stream, but allow enqueued tracks to play to completion
		DisconnectNamedPipe(ctx->hPipe);
		CloseHandle(ctx->hPipe);
		if (ov.hEvent) CloseHandle(ov.hEvent);
		g_activeClients.fetch_sub(1);
		return 0;
	}

	unsigned __stdcall AcceptThread(void*)
	{
		OVERLAPPED ov = {};
		ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (!ov.hEvent)
		{
			g_bPipeRunning.store(false);
			return 0;
		}

		while (g_bPipeRunning)
		{
			HANDLE hPipe = CreateNamedPipeA(
				VOICE_EXTENSION_PIPE_NAME,
				PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
				PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
				PIPE_UNLIMITED_INSTANCES, 65536, 65536, 1000, nullptr);

			if (hPipe == INVALID_HANDLE_VALUE)
			{
				if (WaitForSingleObject(g_hStopEvent, 250) == WAIT_OBJECT_0) break;
				continue;
			}

			ResetEvent(ov.hEvent);
			BOOL connected = ConnectNamedPipe(hPipe, &ov);
			DWORD err = connected ? ERROR_SUCCESS : GetLastError();
			DWORD n = 0;

			if (!connected && err == ERROR_IO_PENDING)
			{
				HANDLE waits[2] = { g_hStopEvent, ov.hEvent };
				if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0)
				{
					CancelIoEx(hPipe, &ov);
					GetOverlappedResult(hPipe, &ov, &n, TRUE);
					CloseHandle(hPipe);
					break;
				}
				connected = GetOverlappedResult(hPipe, &ov, &n, FALSE);
			}
			else if (!connected && err == ERROR_PIPE_CONNECTED)
			{
				connected = TRUE;
			}

			if (!connected || g_activeClients.load() >= kMaxClients)
			{
				DisconnectNamedPipe(hPipe);
				CloseHandle(hPipe);
				continue;
			}

			g_activeClients.fetch_add(1);
			uint32_t cid = VoiceCore::Get().RegisterClient("PipeClient");
			if (cid == 0)
			{
				g_activeClients.fetch_sub(1);
				DisconnectNamedPipe(hPipe);
				CloseHandle(hPipe);
				continue;
			}
			ClientContext* ctx = nullptr;
			try
			{
				ctx = new ClientContext{ hPipe, cid };
			}
			catch (...)
			{
				ctx = nullptr;
			}

			if (!ctx)
			{
				VoiceCore::Get().UnregisterClient(cid);
				g_activeClients.fetch_sub(1);
				DisconnectNamedPipe(hPipe);
				CloseHandle(hPipe);
				continue;
			}

			const uintptr_t th = _beginthreadex(nullptr, 0, ClientThread, ctx, 0, nullptr);
			if (!th)
			{
				VoiceCore::Get().UnregisterClient(cid);
				g_activeClients.fetch_sub(1);
				delete ctx;
				DisconnectNamedPipe(hPipe);
				CloseHandle(hPipe);
				continue;
			}
			CloseHandle(reinterpret_cast<HANDLE>(th));
		}

		CloseHandle(ov.hEvent);
		return 0;
	}
}

void VoicePipe_StartServer()
{
	if (g_bPipeRunning.exchange(true))
		return;

	DebuLog("VoicePipe_StartServer: starting named pipe server (\\\\.\\pipe\\VoiceExtension)");

	// Pin the DLL in memory so client threads unwinding during plugin shutdown don't crash into unmapped memory
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		reinterpret_cast<LPCSTR>(&VoicePipe_StartServer), &self);

	if (g_hStopEvent)
		ResetEvent(g_hStopEvent);
	else
		g_hStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

	if (!g_hStopEvent)
	{
		g_bPipeRunning.store(false);
		DebuLog("VoicePipe_StartServer: CreateEvent failed");
		return;
	}

	g_hAcceptThread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, AcceptThread, nullptr, 0, nullptr));
	if (!g_hAcceptThread)
	{
		g_bPipeRunning.store(false);
		DebuLog("VoicePipe_StartServer: _beginthreadex failed");
		if (g_activeClients.load() == 0)
		{
			CloseHandle(g_hStopEvent);
			g_hStopEvent = nullptr;
		}
	}
}

void VoicePipe_StopServer()
{
	const bool wasRunning = g_bPipeRunning.exchange(false);
	if (!wasRunning && !g_hAcceptThread && !g_hStopEvent)
		return;

	DebuLog("VoicePipe_StopServer: stopping named pipe server");

	if (g_hStopEvent)
		SetEvent(g_hStopEvent);

	if (g_hAcceptThread)
	{
		WaitForSingleObject(g_hAcceptThread, 2000);
		CloseHandle(g_hAcceptThread);
		g_hAcceptThread = nullptr;
	}

	for (int i = 0; i < 200 && g_activeClients.load() > 0; ++i)
		Sleep(10);

	if (g_hStopEvent)
	{
		// Client threads may still be blocked on this event. Closing it while they
		// are alive is a use-after-close; keep it for reuse by the next StartServer.
		if (g_activeClients.load() == 0)
		{
			CloseHandle(g_hStopEvent);
			g_hStopEvent = nullptr;
		}
		else
		{
			DebuLog("VoicePipe_StopServer: clients still active, keeping stop event open");
		}
	}
}

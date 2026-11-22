#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <tlhelp32.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Audio.h"

struct LoopbackTarget
{
	DWORD TargetProcessId;
	int   Mode;
};

struct LoopbackActivation
{
	int            ActivationType;
	LoopbackTarget Target;
};

static const wchar_t* ProcessLoopbackDevice = L"VAD\\Process_Loopback";

static std::atomic<bool>   s_Running{ false };
static std::thread         s_Thread;
static std::atomic<double> s_LastBeat{ -100.0 };
static std::atomic<bool>   s_Active{ false };
static std::mutex          s_Mutex;
static std::string         s_Source;
static std::string         s_Status = "off";

static double NowSeconds()
{
	static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart / (double)freq.QuadPart;
}

static void SetStatus(const std::string& text)
{
	std::lock_guard<std::mutex> lock(s_Mutex);
	s_Status = text;
}

class ActivationHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
{
public:
	ActivationHandler() { m_Done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
	virtual ~ActivationHandler()
	{
		if (m_Client) { m_Client->Release(); }
		if (m_Done) { CloseHandle(m_Done); }
	}

	ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_Refs); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		LONG refs = InterlockedDecrement(&m_Refs);
		if (refs == 0) { delete this; }
		return (ULONG)refs;
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
	{
		if (!out) { return E_POINTER; }
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
		{
			*out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
		}
		else if (riid == __uuidof(IAgileObject))
		{
			*out = static_cast<IAgileObject*>(this);
		}
		else
		{
			*out = nullptr;
			return E_NOINTERFACE;
		}
		AddRef();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
	{
		HRESULT result = E_FAIL;
		IUnknown* unknown = nullptr;
		if (SUCCEEDED(operation->GetActivateResult(&result, &unknown)) && SUCCEEDED(result) && unknown)
		{
			unknown->QueryInterface(__uuidof(IAudioClient), (void**)&m_Client);
		}
		if (unknown) { unknown->Release(); }
		SetEvent(m_Done);
		return S_OK;
	}

	HANDLE       m_Done = nullptr;
	IAudioClient* m_Client = nullptr;

private:
	LONG m_Refs = 1;
};

static std::string Lower(std::string s)
{
	for (char& c : s) { if (c >= 'A' && c <= 'Z') { c = (char)(c - 'A' + 'a'); } }
	return s;
}

static DWORD FindSourceProcess(const std::string& appId)
{
	std::string want = Lower(appId);
	size_t slash = want.find_last_of("\\/!");
	if (slash != std::string::npos) { want = want.substr(slash + 1); }
	if (want.empty()) { return 0; }
	if (want.size() < 4 || want.substr(want.size() - 4) != ".exe") { want += ".exe"; }

	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) { return 0; }
	std::vector<PROCESSENTRY32W> list;
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snap, &entry))
	{
		do { list.push_back(entry); } while (Process32NextW(snap, &entry));
	}
	CloseHandle(snap);

	auto nameOf = [](const PROCESSENTRY32W& e)
	{
		char buffer[MAX_PATH];
		WideCharToMultiByte(CP_UTF8, 0, e.szExeFile, -1, buffer, sizeof(buffer), nullptr, nullptr);
		return Lower(buffer);
	};
	DWORD fallback = 0;
	for (const auto& e : list)
	{
		if (nameOf(e) != want) { continue; }
		if (!fallback) { fallback = e.th32ProcessID; }
		bool parentSame = false;
		for (const auto& p : list)
		{
			if (p.th32ProcessID == e.th32ParentProcessID && nameOf(p) == want) { parentSame = true; break; }
		}
		if (!parentSame) { return e.th32ProcessID; }
	}
	return fallback;
}

static IAudioClient* OpenProcessClient(DWORD pid)
{
	LoopbackActivation params{};
	params.ActivationType = 1;
	params.Target.TargetProcessId = pid;
	params.Target.Mode = 0;

	PROPVARIANT prop;
	PropVariantInit(&prop);
	prop.vt = VT_BLOB;
	prop.blob.cbSize = sizeof(params);
	prop.blob.pBlobData = (BYTE*)&params;

	ActivationHandler* handler = new ActivationHandler();
	IActivateAudioInterfaceAsyncOperation* operation = nullptr;
	IAudioClient* client = nullptr;
	if (SUCCEEDED(ActivateAudioInterfaceAsync(ProcessLoopbackDevice, __uuidof(IAudioClient), &prop, handler, &operation)))
	{
		if (WaitForSingleObject(handler->m_Done, 3000) == WAIT_OBJECT_0 && handler->m_Client)
		{
			client = handler->m_Client;
			client->AddRef();
		}
	}
	if (operation) { operation->Release(); }
	handler->Release();
	return client;
}

static IAudioClient* OpenSystemClient()
{
	IMMDeviceEnumerator* enumerator = nullptr;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator))) { return nullptr; }
	IMMDevice* device = nullptr;
	IAudioClient* client = nullptr;
	if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
	{
		device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
		device->Release();
	}
	enumerator->Release();
	return client;
}

static void CaptureSession(IAudioClient* client, bool processMode, const std::string& sourceAtStart)
{
	WAVEFORMATEX format{};
	format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
	format.nChannels = 2;
	format.nSamplesPerSec = 48000;
	format.wBitsPerSample = 32;
	format.nBlockAlign = (WORD)(format.nChannels * format.wBitsPerSample / 8);
	format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

	DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
	if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &format, nullptr))) { SetStatus("could not start audio capture"); return; }
	IAudioCaptureClient* capture = nullptr;
	if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture)) || !capture) { SetStatus("could not start audio capture"); return; }
	if (FAILED(client->Start())) { capture->Release(); SetStatus("could not start audio capture"); return; }

	SetStatus(processMode ? "listening to the music app" : "listening to all sound");
	s_Active = true;

	const float sampleRate = 48000.0f;
	const float alpha = 1.0f - std::exp(-2.0f * 3.14159265f * 150.0f / sampleRate);
	float low = 0.0f;
	float energy = 0.0f;
	int hopCount = 0;
	const int hop = 1024;
	float history[43] = {};
	int historyIndex = 0;
	int historyFilled = 0;
	double lastBeat = 0.0;
	DWORD lastSourceCheck = GetTickCount();

	while (s_Running)
	{
		Sleep(10);
		if (GetTickCount() - lastSourceCheck > 2000)
		{
			lastSourceCheck = GetTickCount();
			std::lock_guard<std::mutex> lock(s_Mutex);
			if (s_Source != sourceAtStart) { break; }
		}
		UINT32 packet = 0;
		while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0)
		{
			BYTE* data = nullptr;
			UINT32 frames = 0;
			DWORD bufferFlags = 0;
			if (FAILED(capture->GetBuffer(&data, &frames, &bufferFlags, nullptr, nullptr))) { break; }
			const float* samples = (const float*)data;
			bool silent = (bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
			for (UINT32 i = 0; i < frames; i++)
			{
				float mono = silent ? 0.0f : (samples[i * 2] + samples[i * 2 + 1]) * 0.5f;
				low += (mono - low) * alpha;
				energy += low * low;
				if (++hopCount >= hop)
				{
					float e = energy / hop;
					energy = 0.0f;
					hopCount = 0;
					float average = 0.0f;
					for (int k = 0; k < historyFilled; k++) { average += history[k]; }
					average = historyFilled > 0 ? average / historyFilled : e;
					double now = NowSeconds();
					if (historyFilled >= 20 && e > average * 1.45f && e > 1e-5f && now - lastBeat > 0.28)
					{
						lastBeat = now;
						s_LastBeat = now;
					}
					history[historyIndex] = e;
					historyIndex = (historyIndex + 1) % 43;
					if (historyFilled < 43) { historyFilled++; }
				}
			}
			capture->ReleaseBuffer(frames);
		}
	}

	client->Stop();
	capture->Release();
	s_Active = false;
}

static void AudioLoop()
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	while (s_Running)
	{
		std::string source;
		{
			std::lock_guard<std::mutex> lock(s_Mutex);
			source = s_Source;
		}
		IAudioClient* client = nullptr;
		bool processMode = false;
		DWORD pid = FindSourceProcess(source);
		if (pid)
		{
			client = OpenProcessClient(pid);
			processMode = client != nullptr;
		}
		if (!client) { client = OpenSystemClient(); }
		if (client)
		{
			CaptureSession(client, processMode, source);
			client->Release();
		}
		else
		{
			SetStatus("no audio device");
		}
		for (int i = 0; i < 20 && s_Running; i++) { Sleep(100); }
	}
	SetStatus("off");
	CoUninitialize();
}

void Audio_Start()
{
	if (s_Running) { return; }
	s_Running = true;
	s_Thread = std::thread(AudioLoop);
}

void Audio_Stop()
{
	if (!s_Running) { return; }
	s_Running = false;
	if (s_Thread.joinable()) { s_Thread.join(); }
	s_Active = false;
}

void Audio_SetSource(const std::string& appId)
{
	std::lock_guard<std::mutex> lock(s_Mutex);
	s_Source = appId;
}

float Audio_Pulse()
{
	if (!s_Active) { return 0.0f; }
	double age = NowSeconds() - s_LastBeat.load();
	if (age < 0.0) { return 0.0f; }
	return (float)std::exp(-age * 7.0);
}

bool Audio_Active()
{
	return s_Active;
}

std::string Audio_Status()
{
	std::lock_guard<std::mutex> lock(s_Mutex);
	return s_Status;
}

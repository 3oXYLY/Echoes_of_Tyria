#include <windows.h>

#include <cstdio>
#include <string>

#include "Nexus.h"

typedef void (*CoreLoadFn)(AddonAPI_t*);
typedef void (*CoreVoidFn)();

static AddonAPI_t* s_API = nullptr;
static HMODULE     s_Core = nullptr;
static CoreVoidFn  s_CoreUnload = nullptr;
static CoreVoidFn  s_CoreRender = nullptr;
static CoreVoidFn  s_CoreOptions = nullptr;
static std::string s_CoreDir;
static std::string s_CorePath;
static FILETIME    s_CoreTime{};
static bool        s_TriedTime = false;
static int         s_Generation = 0;
static DWORD       s_LastCheck = 0;

static bool FileTime(const std::string& path, FILETIME& ft)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) { return false; }
	ft = data.ftLastWriteTime;
	return true;
}

static bool FileReady(const std::string& path)
{
	HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) { return false; }
	CloseHandle(h);
	return true;
}

static void DeleteOldCopies()
{
	WIN32_FIND_DATAA data;
	HANDLE find = FindFirstFileA((s_CoreDir + "\\run_*.dll").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) { return; }
	do
	{
		DeleteFileA((s_CoreDir + "\\" + data.cFileName).c_str());
	} while (FindNextFileA(find, &data));
	FindClose(find);
}

static void UnloadCore()
{
	if (s_CoreUnload) { s_CoreUnload(); }
	if (s_Core) { FreeLibrary(s_Core); }
	s_Core = nullptr;
	s_CoreUnload = nullptr;
	s_CoreRender = nullptr;
	s_CoreOptions = nullptr;
}

static void Alert(const char* text)
{
	if (s_API) { s_API->GUI_SendAlert(text); }
}

static void LoadCore()
{
	FILETIME ft;
	if (!FileTime(s_CorePath, ft)) { return; }
	s_CoreTime = ft;
	s_TriedTime = true;

	DeleteOldCopies();
	s_Generation++;
	char name[64];
	snprintf(name, sizeof(name), "\\run_%lu_%d.dll", GetCurrentProcessId(), s_Generation);
	std::string copy = s_CoreDir + name;
	if (!CopyFileA(s_CorePath.c_str(), copy.c_str(), FALSE)) { Alert("Lyric HUD: could not copy the core file"); return; }

	HMODULE module = LoadLibraryA(copy.c_str());
	if (!module) { Alert("Lyric HUD: could not load the core file"); return; }
	CoreLoadFn load = (CoreLoadFn)GetProcAddress(module, "CoreLoad");
	CoreVoidFn unload = (CoreVoidFn)GetProcAddress(module, "CoreUnload");
	CoreVoidFn render = (CoreVoidFn)GetProcAddress(module, "CoreRender");
	CoreVoidFn options = (CoreVoidFn)GetProcAddress(module, "CoreOptions");
	if (!load || !unload || !render || !options)
	{
		FreeLibrary(module);
		Alert("Lyric HUD: the core file is missing functions");
		return;
	}
	s_Core = module;
	s_CoreUnload = unload;
	s_CoreRender = render;
	s_CoreOptions = options;
	load(s_API);
}

static void LoaderPreRender()
{
	DWORD now = GetTickCount();
	if (now - s_LastCheck < 1000) { return; }
	s_LastCheck = now;
	FILETIME ft;
	if (!FileTime(s_CorePath, ft)) { return; }
	if (s_TriedTime && CompareFileTime(&ft, &s_CoreTime) == 0) { return; }
	if (!FileReady(s_CorePath)) { return; }
	UnloadCore();
	LoadCore();
}

static void LoaderRender()
{
	if (s_CoreRender) { s_CoreRender(); }
}

static void LoaderOptions()
{
	if (s_CoreOptions) { s_CoreOptions(); }
}

static void AddonLoad(AddonAPI_t* aApi)
{
	s_API = aApi;
	const char* base = s_API->Paths_GetAddonDirectory("LyricHud");
	if (base)
	{
		CreateDirectoryA(base, nullptr);
		s_CoreDir = std::string(base) + "\\core";
		CreateDirectoryA(s_CoreDir.c_str(), nullptr);
		s_CorePath = s_CoreDir + "\\LyricHudCore.dll";
	}
	s_TriedTime = false;
	s_LastCheck = 0;
	LoadCore();
	if (!s_Core) { Alert("Lyric HUD: core file not found yet, waiting for it"); }
	s_API->GUI_Register(RT_PreRender, LoaderPreRender);
	s_API->GUI_Register(RT_Render, LoaderRender);
	s_API->GUI_Register(RT_OptionsRender, LoaderOptions);
}

static void AddonUnload()
{
	s_API->GUI_Deregister(LoaderOptions);
	s_API->GUI_Deregister(LoaderRender);
	s_API->GUI_Deregister(LoaderPreRender);
	UnloadCore();
	s_API = nullptr;
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
	static AddonDefinition_t def{};
	def.Signature        = 0x7A3F19C2;
	def.APIVersion       = NEXUS_API_VERSION;
	def.Name             = "Echoes of Tyria (dev)";
	def.Version.Major    = 1;
	def.Version.Minor    = 0;
	def.Version.Build    = 0;
	def.Version.Revision = 0;
	def.Author           = "Foxy";
	def.Description      = "Shows the synced lyrics of the song you are playing as 3D text in the game world.";
	def.Load             = AddonLoad;
	def.Unload           = AddonUnload;
	def.Flags            = AF_None;
	return &def;
}

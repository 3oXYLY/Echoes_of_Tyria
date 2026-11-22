#include <windows.h>
#include <dxgi.h>
#include <d3d11.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <vector>

#include "Nexus.h"
#include "Mumble.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "json.hpp"
#include "Media.h"
#include "Lyrics.h"
#include "Icons.h"
#include "Depth.h"
#include "Occlusion.h"
#include "Cover.h"
#include "Audio.h"
#ifdef LYRICHUD_RELEASE
#include "FontPack.h"
#endif

#define LYRICHUD_VERSION "1.0.0"
#define ADDON_NAME "Echoes of Tyria"
#ifdef LYRICHUD_RELEASE
#define ADDON_FOLDER "EchoesOfTyria"
#else
#define ADDON_FOLDER "LyricHud"
#endif

struct V3
{
	float x, y, z;
};

static V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 Mul(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static float Len(V3 a) { return std::sqrt(Dot(a, a)); }
static V3 Norm(V3 a)
{
	float l = Len(a);
	if (l < 1e-6f) { return a; }
	return Mul(a, 1.0f / l);
}
static V3 Horizontal(V3 a) { a.y = 0.0f; return Norm(a); }
static V3 ToV3(const Mumble::Vector3& v) { return { v.X, v.Y, v.Z }; }
static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float EaseOut(float t) { float u = 1.0f - t; return 1.0f - u * u * u; }
static float EaseOutBack(float t)
{
	const float c1 = 1.70158f;
	const float c3 = c1 + 1.0f;
	float u = t - 1.0f;
	return 1.0f + c3 * u * u * u + c1 * u * u;
}
static float EaseOutBounce(float t)
{
	const float n1 = 7.5625f;
	const float d1 = 2.75f;
	if (t < 1.0f / d1) { return n1 * t * t; }
	if (t < 2.0f / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
	if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
	t -= 2.625f / d1;
	return n1 * t * t + 0.984375f;
}
static float Hash01(unsigned a, unsigned b)
{
	unsigned h = a * 2654435761u ^ (b + 0x9E3779B9u) * 2246822519u;
	h ^= h >> 15;
	h *= 2246822519u;
	h ^= h >> 13;
	return (float)(h & 0xFFFFFF) / 16777215.0f;
}

static const V3 WorldUp = { 0.0f, 1.0f, 0.0f };
static const float Pi = 3.14159265f;

static const char* KeybindToggle  = "KB_LYRICHUD_TOGGLE";
static const char* KeybindDepth   = "KB_LYRICHUD_DEPTHWINDOW";
static const char* KeybindTest    = "KB_LYRICHUD_TEST";
static const char* KeybindPlay     = "KB_LYRICHUD_PLAY";
static const char* KeybindNext     = "KB_LYRICHUD_SKIPNEXT";
static const char* KeybindPrev     = "KB_LYRICHUD_SKIPBACK";
static const char* KeybindEarlier  = "KB_LYRICHUD_EARLIER";
static const char* KeybindLater    = "KB_LYRICHUD_LATER";
static std::atomic<int> s_NudgeRequested{ 0 };
static const char* ShortcutId     = "SC_LYRICHUD";
static const char* IconId         = "ICON_LYRICHUD";
static const char* IconHoverId    = "ICON_LYRICHUD_HOVER";
static const char* GameFontName   = "Game font";

enum AppearMode    { Appear_Fade = 0, Appear_FadeRise, Appear_Letters, Appear_Scatter, Appear_Bounce, Appear_Spin, Appear_Pop, Appear_Drop, Appear_Zoom, Appear_Typewriter, Appear_WaveIn, Appear_Glitch, Appear_FromBehind, Appear_SlideIn, Appear_Flip, Appear_Elastic, Appear_RainDown, Appear_Swirl, Appear_Count };
enum IdleMode      { Idle_None = 0, Idle_Float, Idle_Wave, Idle_Pulse, Idle_Shake, Idle_Swing, Idle_Jelly, Idle_Glitch, Idle_Count };
enum DisappearMode { Disappear_FadeUp = 0, Disappear_Fade, Disappear_Scatter, Disappear_Shrink, Disappear_Drop, Disappear_SpinOut, Disappear_BlowAway, Disappear_Burst, Disappear_Letters, Disappear_Sink, Disappear_SlideOut, Disappear_FlipOut, Disappear_Melt, Disappear_Shatter, Disappear_SwirlOut, Disappear_Count };
enum PlaceMode     { Place_Scattered = 0, Place_Near };
enum ParticleStyle { Particles_Sparkles = 0, Particles_Confetti, Particles_Embers, Particles_Bubbles, Particles_Snow, Particles_Stars, Particles_Hearts, Particles_Notes, Particles_Petals, Particles_Fireflies, Particles_Glitter, Particles_Rings, Particles_Leaves, Particles_Butterflies, Particles_Smoke, Particles_Pixels, Particles_Rain, Particles_Count };
enum LandEffect    { Land_None = 0, Land_Flash, Land_Ring, Land_Sparks, Land_Bounce, Land_Count };

typedef std::array<float, 4> Color4;

struct Settings
{
	bool  showLyrics     = true;
	float lyricsOffset   = 0.0f;
	float maxOnScreen    = 8.0f;
	bool  splitLong      = true;
	int   maxChars       = 28;

	int   placeMode      = Place_Scattered;
	float aheadMin       = 2.0f;
	float aheadMax       = 25.0f;
	float screenMarginX  = 0.18f;
	float screenTop      = 0.08f;
	float screenBottom   = 0.55f;
	float distance       = 5.0f;
	float height         = 2.0f;
	float spreadSide     = 1.5f;
	float spreadUp       = 0.6f;

	float worldHeight    = 0.8f;
	bool  autoScale      = true;
	float refDistance    = 10.0f;
	bool  readableBack   = false;

	int   appearMode     = Appear_Letters;
	float appearTime     = 0.5f;
	float letterDelay    = 0.03f;
	int   idleMode       = Idle_None;
	float idleStrength   = 1.0f;
	int   disappearMode  = Disappear_FadeUp;
	float disappearTime  = 0.8f;

	std::string fontName = "Segoe UI Bold";
	Color4 textColor     = { 1.0f, 1.0f, 1.0f, 1.0f };
	bool   karaoke       = false;
	bool   coverColors   = false;
	bool   sparkles      = false;
	int    particleStyle = Particles_Sparkles;
	bool   particlesContinuous = false;
	float  particleAmount = 1.0f;
	bool   beatPulse     = false;
	float  beatAmount    = 0.08f;
	Color4 karaokeColor  = { 1.0f, 0.82f, 0.3f, 1.0f };
	bool   extrude       = true;
	bool   autoLayers    = true;
	int    extrudeLayers = 6;
	int    maxLayers     = 64;
	float  extrudeDepth  = 0.25f;
	Color4 depthColor    = { 0.59f, 0.59f, 0.69f, 1.0f };
	bool   shadow        = true;
	Color4 shadowColor   = { 0.0f, 0.0f, 0.0f, 0.67f };
	bool   outline       = false;
	float  outlineWidth  = 0.04f;
	Color4 outlineColor  = { 0.0f, 0.0f, 0.0f, 1.0f };
	bool   glow          = false;
	float  glowSize      = 0.15f;
	Color4 glowColor     = { 1.0f, 0.8f, 0.35f, 0.6f };

	bool  occlusion      = true;
	bool  liftLetters    = true;
	float nearPlane      = 0.288f;
	bool  autoNearPlane  = true;
	bool  tiltToCamera   = true;
	bool  behindUi       = true;
	bool  followMovement = false;
	bool  hideInCombat   = false;
	bool  hideInPvP      = false;
	bool  hideWhileTyping = false;
	bool  titleCard      = false;
	bool  coverArt       = false;
	bool  wordByWord     = false;
	int   wordMode       = 0;
	bool  splitRepeats   = true;
	bool  censor         = false;
	int   keepLines      = 1;
	bool  splitBacking   = false;
	std::map<std::string, float> songOffsets;
	std::map<std::string, float> nearByMap;
	std::map<std::string, float> depthKByMap;
	bool  smartPlacement = true;
	bool  useWalls       = true;
	float wallChance     = 0.6f;
	bool  avoidCharacter = true;
	bool  relocate       = true;
	bool  showDebug      = false;
	bool  pinToScreen    = false;
	bool  faceMe         = false;
	float mountFarther   = 1.8f;
	bool  otherVoiceColor = true;
	bool  karaokeFromCover = true;
	bool  chorusBoost    = true;
	int   landEffect     = Land_None;
};

static Settings S;

struct TimedLine
{
	double      time;
	std::string text;
	bool        chorus = false;
};

struct GlyphBox
{
	float lx;
	float ly;
	float hw;
	float hh;
};

enum SpawnKind { Spawn_Air = 0, Spawn_Wall, Spawn_Close };

struct SpawnedLine
{
	std::string text;
	V3       anchor;
	V3       right;
	V3       back;
	V3       up = { 0, 1, 0 };
	float    height;
	double   start;
	double   fadeStart;
	unsigned seed;
	unsigned id;
	bool     onWall = false;
	float    blockedTime = 0.0f;
	int      relocations = 0;
	int      kind = Spawn_Air;
	int      spawnClear = 0;
	float    singDuration = 0.0f;
	int      glyphCount = 0;
	bool     isTitle = false;
	double   lyricStart = -1.0;
	int      group = -1;
	double   lyricEnd = -1.0;
	std::vector<float> wordFrac;
	int      coverVersion = -1;
	std::vector<V3> lift;
	std::vector<V3> liftTarget;
	std::vector<GlyphBox> glyphs;
	const void* layoutFont = nullptr;
	bool     settled = false;
	double   settleDeadline = 0.0;
	int      voice = 0;
	bool     chorus = false;
	bool     pinned = false;
	V3       camAnchor = { 0, 0, 0 };
	V3       camRight = { 1, 0, 0 };
	V3       camUp = { 0, 1, 0 };
	V3       camBack = { 0, 0, 1 };
};

struct FontEntry
{
	std::string name;
	std::string id;
	std::string path;
	ImFont*     font = nullptr;
	bool        requested = false;
};

struct CameraBasis
{
	V3    pos;
	V3    f;
	V3    r;
	V3    u;
	float tanHalf;
	float aspect;
	float width;
	float height;
};

static AddonAPI_t*       s_API      = nullptr;
static NexusLinkData_t*  s_Nexus    = nullptr;
static Mumble::Data*     s_Mumble   = nullptr;
static Mumble::Identity* s_Identity = nullptr;

static CameraBasis              s_Cam;
static std::vector<SpawnedLine> s_Lines;
static std::vector<TimedLine>   s_Expanded;
static std::vector<FontEntry>   s_Fonts;
static const void*  s_ExpandedSource   = nullptr;
static bool         s_ExpandedSplit    = false;
static int          s_ExpandedMaxChars = 0;
static std::string  s_LastTrackKey;
static int          s_LastIndex     = -2;
static bool         s_TestRequested = false;
static std::atomic<bool> s_ToggleRequested{ false };
static std::atomic<bool> s_DepthWindowRequested{ false };
static std::atomic<bool> s_TestKeyRequested{ false };
static unsigned     s_Rng           = 0x9E3779B9u;
static unsigned     s_NextLineId    = 1;
static float        s_NearPlane     = 0.288f;
static float        s_DepthK        = 0.0f;
static float        s_MeasuredNear  = 0.0f;
static bool         s_DepthWindow   = false;
static bool         s_DepthFailed   = false;
static int          s_ProbeCount    = 0;
static int          s_LiftedCount   = 0;
static int          s_CheckBatches  = 0;
static int          s_CheckMatches  = 0;
static uint32_t     s_LastMapId     = 0;
static bool         s_WasHidden     = false;
static bool         s_TitleShown    = false;
static double       s_LivePosition  = 0.0;
struct PlannedSpawn
{
	double      time;
	double      end;
	std::string text;
	float       scale;
	int         voice = 0;
};
static std::vector<PlannedSpawn> s_Plan;
static size_t       s_PlanNext      = 0;
static int          s_Group         = 0;
static V3           s_MoveVel       = { 0, 0, 0 };
static V3           s_PrevAvatar    = { 0, 0, 0 };
static bool         s_HavePrevAvatar = false;
static Color4       s_LiveText      = { 1.0f, 1.0f, 1.0f, 1.0f };
static Color4       s_LiveKaraoke   = { 1.0f, 0.82f, 0.3f, 1.0f };
static bool         s_PlanChorus    = false;
static bool         s_EverHadTrack  = false;
static Color4       s_LiveDepth     = { 0.5f, 0.5f, 0.5f, 1.0f };
static bool         s_WasGameplay   = false;
static bool         s_Dirty         = false;
static double       s_DirtyTime     = 0.0;
static std::string  s_AddonDir;
static std::string  s_SettingsPath;
static FILETIME     s_SettingsTime  = {};
static double       s_SettingsCheck = 0.0;

static bool SettingsFileTime(FILETIME& out)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExA(s_SettingsPath.c_str(), GetFileExInfoStandard, &data)) { return false; }
	out = data.ftLastWriteTime;
	return true;
}
static std::string  s_FontsDir;
static std::string  s_LyricsDir;

static const ImWchar FontRanges[] = { 0x0020, 0x00FF, 0x2010, 0x2027, 0 };
static ImFontConfig  s_FontConfig;

struct ScriptFont
{
	ImFontAtlas*              atlas = nullptr;
	ImFont*                   font = nullptr;
	ID3D11ShaderResourceView* view = nullptr;
	std::set<unsigned>        chars;
};
struct ScriptBuild
{
	ImFontAtlas*         atlas = nullptr;
	std::vector<ImWchar> ranges;
	std::set<unsigned>   chars;
	unsigned char*       pixels = nullptr;
	int                  width = 0;
	int                  height = 0;
	bool                 ok = false;
};
static ScriptFont        s_Script;
static std::set<unsigned> s_ScriptWanted;
static std::thread       s_ScriptThread;
static std::mutex        s_ScriptLock;
static ScriptBuild*      s_ScriptDone = nullptr;
static bool              s_ScriptBusy = false;
static const void*       s_ScriptSource = nullptr;
static std::string       s_ScriptTitle;

static void ReceiveFont(const char* aIdentifier, void* aFont)
{
	for (auto& entry : s_Fonts)
	{
		if (!entry.id.empty() && entry.id == aIdentifier) { entry.font = (ImFont*)aFont; }
	}
}

static void ReceiveTexture(const char* aIdentifier, Texture_t* aTexture) {}

template <typename T>
static void ReadValue(const nlohmann::json& j, const char* key, T& value)
{
	auto it = j.find(key);
	if (it == j.end()) { return; }
	try { value = it->get<T>(); }
	catch (...) {}
}

static void LoadSettings()
{
	SettingsFileTime(s_SettingsTime);
	std::ifstream in(s_SettingsPath);
	if (!in) { return; }
	nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
	if (j.is_discarded() || !j.is_object()) { return; }

	ReadValue(j, "showLyrics", S.showLyrics);
	ReadValue(j, "lyricsOffset", S.lyricsOffset);
	ReadValue(j, "maxOnScreen", S.maxOnScreen);
	ReadValue(j, "splitLong", S.splitLong);
	ReadValue(j, "maxChars", S.maxChars);
	ReadValue(j, "placeMode", S.placeMode);
	ReadValue(j, "aheadMin", S.aheadMin);
	ReadValue(j, "aheadMax", S.aheadMax);
	ReadValue(j, "screenMarginX", S.screenMarginX);
	ReadValue(j, "screenTop", S.screenTop);
	ReadValue(j, "screenBottom", S.screenBottom);
	ReadValue(j, "distance", S.distance);
	ReadValue(j, "height", S.height);
	ReadValue(j, "spreadSide", S.spreadSide);
	ReadValue(j, "spreadUp", S.spreadUp);
	ReadValue(j, "worldHeight", S.worldHeight);
	ReadValue(j, "autoScale", S.autoScale);
	ReadValue(j, "refDistance", S.refDistance);
	ReadValue(j, "readableBack", S.readableBack);
	ReadValue(j, "appearMode", S.appearMode);
	ReadValue(j, "appearTime", S.appearTime);
	ReadValue(j, "letterDelay", S.letterDelay);
	ReadValue(j, "idleMode", S.idleMode);
	ReadValue(j, "idleStrength", S.idleStrength);
	ReadValue(j, "disappearMode", S.disappearMode);
	ReadValue(j, "disappearTime", S.disappearTime);
	ReadValue(j, "fontName", S.fontName);
	ReadValue(j, "textColor", S.textColor);
	ReadValue(j, "karaoke", S.karaoke);
	ReadValue(j, "coverColors", S.coverColors);
	ReadValue(j, "sparkles", S.sparkles);
	ReadValue(j, "particleStyle", S.particleStyle);
	ReadValue(j, "particlesContinuous", S.particlesContinuous);
	ReadValue(j, "particleAmount", S.particleAmount);
	if (S.particleStyle < 0 || S.particleStyle >= Particles_Count) { S.particleStyle = Particles_Sparkles; }
	ReadValue(j, "beatPulse", S.beatPulse);
	ReadValue(j, "beatAmount", S.beatAmount);
	ReadValue(j, "karaokeColor", S.karaokeColor);
	ReadValue(j, "extrude", S.extrude);
	ReadValue(j, "autoLayers", S.autoLayers);
	ReadValue(j, "extrudeLayers", S.extrudeLayers);
	ReadValue(j, "maxLayers", S.maxLayers);
	ReadValue(j, "extrudeDepth", S.extrudeDepth);
	ReadValue(j, "depthColor", S.depthColor);
	ReadValue(j, "shadow", S.shadow);
	ReadValue(j, "shadowColor", S.shadowColor);
	ReadValue(j, "outline", S.outline);
	ReadValue(j, "outlineWidth", S.outlineWidth);
	ReadValue(j, "outlineColor", S.outlineColor);
	ReadValue(j, "glow", S.glow);
	ReadValue(j, "glowSize", S.glowSize);
	ReadValue(j, "glowColor", S.glowColor);
	ReadValue(j, "occlusion", S.occlusion);
	ReadValue(j, "liftLetters", S.liftLetters);
	ReadValue(j, "nearPlane", S.nearPlane);
	ReadValue(j, "autoNearPlane", S.autoNearPlane);
	ReadValue(j, "tiltToCamera", S.tiltToCamera);
	ReadValue(j, "behindUi", S.behindUi);
	ReadValue(j, "followMovement", S.followMovement);
	ReadValue(j, "hideInCombat", S.hideInCombat);
	ReadValue(j, "hideInPvP", S.hideInPvP);
	ReadValue(j, "hideWhileTyping", S.hideWhileTyping);
	ReadValue(j, "titleCard", S.titleCard);
	ReadValue(j, "coverArt", S.coverArt);
	ReadValue(j, "wordByWord", S.wordByWord);
	ReadValue(j, "wordMode", S.wordMode);
	ReadValue(j, "splitRepeats", S.splitRepeats);
	ReadValue(j, "censor", S.censor);
	if (!j.contains("wordMode") && S.wordByWord) { S.wordMode = 1; }
	if (S.wordMode < 0 || S.wordMode > 2) { S.wordMode = 0; }
	ReadValue(j, "keepLines", S.keepLines);
	ReadValue(j, "splitBacking", S.splitBacking);
	ReadValue(j, "songOffsets", S.songOffsets);
	ReadValue(j, "nearByMap", S.nearByMap);
	ReadValue(j, "depthKByMap", S.depthKByMap);
	int nearVersion = 0;
	ReadValue(j, "nearVersion", nearVersion);
	if (nearVersion < 4) { S.nearByMap.clear(); S.depthKByMap.clear(); s_Dirty = true; s_DirtyTime = 0.0; }
	if (S.nearPlane > 0.385f && S.nearPlane < 0.40f) { S.nearPlane = 0.288f; }
	ReadValue(j, "smartPlacement", S.smartPlacement);
	ReadValue(j, "useWalls", S.useWalls);
	ReadValue(j, "wallChance", S.wallChance);
	ReadValue(j, "avoidCharacter", S.avoidCharacter);
	ReadValue(j, "relocate", S.relocate);
	ReadValue(j, "showDebug", S.showDebug);
	ReadValue(j, "pinToScreen", S.pinToScreen);
	ReadValue(j, "faceMe", S.faceMe);
	ReadValue(j, "mountFarther", S.mountFarther);
	ReadValue(j, "otherVoiceColor", S.otherVoiceColor);
	ReadValue(j, "karaokeFromCover", S.karaokeFromCover);
	ReadValue(j, "chorusBoost", S.chorusBoost);
	ReadValue(j, "landEffect", S.landEffect);
	if (S.landEffect < 0 || S.landEffect >= Land_Count) { S.landEffect = Land_None; }
	if (S.mountFarther < 1.0f || S.mountFarther > 3.0f) { S.mountFarther = 1.8f; }

	if (S.appearMode < 0 || S.appearMode >= Appear_Count) { S.appearMode = Appear_Letters; }
	if (S.idleMode < 0 || S.idleMode >= Idle_Count) { S.idleMode = Idle_None; }
	if (S.disappearMode < 0 || S.disappearMode >= Disappear_Count) { S.disappearMode = Disappear_FadeUp; }
}

static void SaveSettings()
{
	if (s_SettingsPath.empty()) { return; }
	nlohmann::json j;
	j["showLyrics"] = S.showLyrics;
	j["lyricsOffset"] = S.lyricsOffset;
	j["maxOnScreen"] = S.maxOnScreen;
	j["splitLong"] = S.splitLong;
	j["maxChars"] = S.maxChars;
	j["placeMode"] = S.placeMode;
	j["aheadMin"] = S.aheadMin;
	j["aheadMax"] = S.aheadMax;
	j["screenMarginX"] = S.screenMarginX;
	j["screenTop"] = S.screenTop;
	j["screenBottom"] = S.screenBottom;
	j["distance"] = S.distance;
	j["height"] = S.height;
	j["spreadSide"] = S.spreadSide;
	j["spreadUp"] = S.spreadUp;
	j["worldHeight"] = S.worldHeight;
	j["autoScale"] = S.autoScale;
	j["refDistance"] = S.refDistance;
	j["readableBack"] = S.readableBack;
	j["appearMode"] = S.appearMode;
	j["appearTime"] = S.appearTime;
	j["letterDelay"] = S.letterDelay;
	j["idleMode"] = S.idleMode;
	j["idleStrength"] = S.idleStrength;
	j["disappearMode"] = S.disappearMode;
	j["disappearTime"] = S.disappearTime;
	j["fontName"] = S.fontName;
	j["textColor"] = S.textColor;
	j["karaoke"] = S.karaoke;
	j["coverColors"] = S.coverColors;
	j["sparkles"] = S.sparkles;
	j["particleStyle"] = S.particleStyle;
	j["particlesContinuous"] = S.particlesContinuous;
	j["particleAmount"] = S.particleAmount;
	j["beatPulse"] = S.beatPulse;
	j["beatAmount"] = S.beatAmount;
	j["karaokeColor"] = S.karaokeColor;
	j["extrude"] = S.extrude;
	j["autoLayers"] = S.autoLayers;
	j["extrudeLayers"] = S.extrudeLayers;
	j["maxLayers"] = S.maxLayers;
	j["extrudeDepth"] = S.extrudeDepth;
	j["depthColor"] = S.depthColor;
	j["shadow"] = S.shadow;
	j["shadowColor"] = S.shadowColor;
	j["outline"] = S.outline;
	j["outlineWidth"] = S.outlineWidth;
	j["outlineColor"] = S.outlineColor;
	j["glow"] = S.glow;
	j["glowSize"] = S.glowSize;
	j["glowColor"] = S.glowColor;
	j["occlusion"] = S.occlusion;
	j["liftLetters"] = S.liftLetters;
	j["nearPlane"] = S.nearPlane;
	j["autoNearPlane"] = S.autoNearPlane;
	j["tiltToCamera"] = S.tiltToCamera;
	j["behindUi"] = S.behindUi;
	j["followMovement"] = S.followMovement;
	j["hideInCombat"] = S.hideInCombat;
	j["hideInPvP"] = S.hideInPvP;
	j["hideWhileTyping"] = S.hideWhileTyping;
	j["titleCard"] = S.titleCard;
	j["coverArt"] = S.coverArt;
	j["wordByWord"] = S.wordMode == 1;
	j["wordMode"] = S.wordMode;
	j["splitRepeats"] = S.splitRepeats;
	j["censor"] = S.censor;
	j["keepLines"] = S.keepLines;
	j["splitBacking"] = S.splitBacking;
	j["songOffsets"] = S.songOffsets;
	j["nearByMap"] = S.nearByMap;
	j["depthKByMap"] = S.depthKByMap;
	j["nearVersion"] = 4;
	j["smartPlacement"] = S.smartPlacement;
	j["useWalls"] = S.useWalls;
	j["wallChance"] = S.wallChance;
	j["avoidCharacter"] = S.avoidCharacter;
	j["relocate"] = S.relocate;
	j["showDebug"] = S.showDebug;
	j["pinToScreen"] = S.pinToScreen;
	j["faceMe"] = S.faceMe;
	j["mountFarther"] = S.mountFarther;
	j["otherVoiceColor"] = S.otherVoiceColor;
	j["karaokeFromCover"] = S.karaokeFromCover;
	j["chorusBoost"] = S.chorusBoost;
	j["landEffect"] = S.landEffect;

	{
		std::ofstream out(s_SettingsPath, std::ios::trunc);
		if (out) { out << j.dump(2); }
	}
	SettingsFileTime(s_SettingsTime);
	s_Dirty = false;
}

static void MarkDirty(bool changed)
{
	if (!changed) { return; }
	s_Dirty = true;
	s_DirtyTime = ImGui::GetTime();
}

static float Rand01()
{
	s_Rng = s_Rng * 1664525u + 1013904223u;
	return (float)((s_Rng >> 8) & 0xFFFFFF) / 16777215.0f;
}

static float RandRange(float a, float b) { return a + (b - a) * Rand01(); }

static float s_CamTurn = 0.0f;
static V3    s_PrevCamF = { 0, 0, 1 };
static V3    s_PrevCamPos = { 0, 0, 0 };

static void UpdateCamera()
{
	s_Cam.pos = ToV3(s_Mumble->CameraPosition);
	s_Cam.f = Norm(ToV3(s_Mumble->CameraFront));
	s_Cam.r = Norm(Cross(WorldUp, s_Cam.f));
	s_Cam.u = Cross(s_Cam.f, s_Cam.r);
	float fov = (s_Identity && s_Identity->FOV > 0.0f) ? s_Identity->FOV : 1.222f;
	s_Cam.tanHalf = std::tan(fov * 0.5f);
	s_Cam.width = (float)s_Nexus->Width;
	s_Cam.height = (float)s_Nexus->Height;
	s_Cam.aspect = s_Cam.width / s_Cam.height;
	float turn = std::acos(std::max(-1.0f, std::min(1.0f, Dot(s_Cam.f, s_PrevCamF))));
	float shift = Len(Sub(s_Cam.pos, s_PrevCamPos));
	float motion = turn + std::min(0.2f, shift * 0.02f);
	s_CamTurn = motion > s_CamTurn ? motion : s_CamTurn * 0.85f + motion * 0.15f;
	s_PrevCamF = s_Cam.f;
	s_PrevCamPos = s_Cam.pos;
}

static bool ProjectWith(const CameraBasis& c, V3 point, float& outX, float& outY, float& outDepth)
{
	V3 d = Sub(point, c.pos);
	float x = Dot(d, c.r);
	float y = Dot(d, c.u);
	float z = Dot(d, c.f);
	if (z <= 0.1f) { return false; }
	outX = (0.5f + x / (z * c.tanHalf * c.aspect) * 0.5f) * c.width;
	outY = (0.5f - y / (z * c.tanHalf) * 0.5f) * c.height;
	outDepth = z;
	return true;
}

static bool Project(V3 point, float& outX, float& outY, float& outDepth)
{
	return ProjectWith(s_Cam, point, outX, outY, outDepth);
}

static void FadeAllLines(double now)
{
	for (auto& line : s_Lines)
	{
		if (line.fadeStart <= 0.0) { line.fadeStart = now; }
	}
}

static CameraBasis        s_GridCam;
static std::vector<float> s_GridNear;
static std::vector<float> s_GridFar;
static std::vector<float> s_GridCenter;
static std::vector<unsigned char> s_GridFlat;
static int   s_GridW = 0;
static int   s_GridH = 0;
static int   s_GridBatch = -1;
static int   s_GridAge = 9999;
static bool  s_GridReady = false;
static int   s_FrameId = 0;
static CameraBasis s_CamHistory[16];
static int   s_CamHistoryId[16];
static int   s_LastSpawnClear = -1;
static int   s_LastSpawnWall = 0;
static int   s_Relocated = 0;

struct WallSession
{
	bool   active = false;
	V3     normal = { 0, 0, 0 };
	V3     lastPoint = { 0, 0, 0 };
	V3     columnTop = { 0, 0, 0 };
	float  lastHeight = 0.0f;
	float  baseHeight = 0.0f;
	float  columnWidth = 0.0f;
	double lastTime = 0.0;
	int    count = 0;
	int    columns = 0;
};

static WallSession s_Wall;

struct ScreenRect
{
	float x1, y1, x2, y2;
	bool  valid;
};

static ScreenRect s_MinimapRect{ 0, 0, 0, 0, false };
static ScreenRect s_CharacterRect{ 0, 0, 0, 0, false };
static ScreenRect s_TrackerRect{ 0, 0, 0, 0, false };

static ImFont* CurrentFont();
static ImFont* FontFor(const std::string& text);
static void EnsureFontLoaded();

static void ResetGrid()
{
	s_GridNear.clear();
	s_GridFar.clear();
	s_GridCenter.clear();
	s_GridFlat.clear();
	s_GridW = 0;
	s_GridH = 0;
	s_GridBatch = -1;
	s_GridAge = 9999;
	s_GridReady = false;
	s_Wall.active = false;
}

static float RawToMeters(float raw)
{
	if (raw >= 0.99999f) { return 1e9f; }
	return s_NearPlane / std::max(1e-6f, 1.0f - raw + s_DepthK);
}

static void LoadGridData(const float* data, int w, int h)
{
	s_GridW = w;
	s_GridH = h;
	size_t count = (size_t)w * h;
	s_GridNear.resize(count);
	s_GridFar.resize(count);
	s_GridCenter.resize(count);
	s_GridFlat.resize(count);
	for (size_t i = 0; i < count; i++)
	{
		float lo = data[i * 4];
		float hi = data[i * 4 + 1];
		float center = data[i * 4 + 2];
		float residual = data[i * 4 + 3];
		s_GridNear[i] = RawToMeters(lo);
		s_GridFar[i] = RawToMeters(hi);
		s_GridCenter[i] = center;
		bool flat = hi < 0.99999f && center < 0.99999f;
		if (flat)
		{
			float z = RawToMeters(center);
			float meters = residual * z * z / s_NearPlane;
			flat = meters < 0.04f + z * 0.008f;
		}
		s_GridFlat[i] = flat ? 1 : 0;
	}
}

static void PullGrid()
{
	s_CamHistory[s_FrameId & 15] = s_Cam;
	s_CamHistoryId[s_FrameId & 15] = s_FrameId;

	const float* data = nullptr;
	int w = 0, h = 0, batch = -1;
	if (Depth_IsInstalled() && Depth_GetGrid(data, w, h, batch) && batch != s_GridBatch && w > 0 && h > 0)
	{
		int slot = batch & 15;
		if (s_CamHistoryId[slot] == batch && s_FrameId - batch < 16)
		{
			s_GridCam = s_CamHistory[slot];
			LoadGridData(data, w, h);
			s_GridReady = true;
		}
		s_GridBatch = batch;
	}
	s_GridAge = s_GridBatch >= 0 ? s_FrameId - s_GridBatch : 9999;
	if (s_GridAge > 30 || !Depth_IsInstalled()) { s_GridReady = false; }
}

static bool GridUsable()
{
	return S.smartPlacement && s_GridReady;
}

static V3 ScreenToWorld(float sx, float sy, float z)
{
	float ndcX = sx / s_Cam.width * 2.0f - 1.0f;
	float ndcY = 1.0f - sy / s_Cam.height * 2.0f;
	float x = ndcX * s_Cam.tanHalf * s_Cam.aspect * z;
	float y = ndcY * s_Cam.tanHalf * z;
	return Add(Add(Add(s_Cam.pos, Mul(s_Cam.r, x)), Mul(s_Cam.u, y)), Mul(s_Cam.f, z));
}

static V3 GridPoint(int cx, int cy, float z)
{
	const CameraBasis& c = s_GridCam;
	float ndcX = (cx + 0.5f) / s_GridW * 2.0f - 1.0f;
	float ndcY = 1.0f - (cy + 0.5f) / s_GridH * 2.0f;
	float x = ndcX * c.tanHalf * c.aspect * z;
	float y = ndcY * c.tanHalf * z;
	return Add(Add(Add(c.pos, Mul(c.r, x)), Mul(c.u, y)), Mul(c.f, z));
}

struct GridHit
{
	int   cx = 0;
	int   cy = 0;
	float gx = 0.0f;
	float gy = 0.0f;
	float pointZ = 0.0f;
	float nearZ = 1e9f;
	float farZ = 1e9f;
};

static bool GridLookup(V3 p, GridHit& h)
{
	if (!s_GridReady) { return false; }
	float sx, sy, z;
	if (!ProjectWith(s_GridCam, p, sx, sy, z)) { return false; }
	h.gx = sx / s_GridCam.width * s_GridW;
	h.gy = sy / s_GridCam.height * s_GridH;
	h.cx = (int)std::floor(h.gx);
	h.cy = (int)std::floor(h.gy);
	if (h.cx < 0 || h.cy < 0 || h.cx >= s_GridW || h.cy >= s_GridH) { return false; }
	int i = h.cy * s_GridW + h.cx;
	h.pointZ = z;
	h.nearZ = s_GridNear[i];
	h.farZ = s_GridFar[i];
	return true;
}

static bool CellRaw(int cx, int cy, float& raw)
{
	if (cx < 0 || cy < 0 || cx >= s_GridW || cy >= s_GridH) { return false; }
	int i = cy * s_GridW + cx;
	if (!s_GridFlat[i]) { return false; }
	raw = s_GridCenter[i];
	return true;
}

static bool CellMid(int cx, int cy, float& z)
{
	float raw;
	if (!CellRaw(cx, cy, raw)) { return false; }
	z = RawToMeters(raw);
	return true;
}

static bool SurfaceDepth(const GridHit& h, float& surfZ)
{
	float rc;
	if (!CellRaw(h.cx, h.cy, rc)) { return false; }
	surfZ = RawToMeters(rc);
	int x0 = (int)std::floor(h.gx - 0.5f);
	int y0 = (int)std::floor(h.gy - 0.5f);
	float tx = h.gx - 0.5f - x0;
	float ty = h.gy - 0.5f - y0;
	float r00, r10, r01, r11;
	if (CellRaw(x0, y0, r00) && CellRaw(x0 + 1, y0, r10) && CellRaw(x0, y0 + 1, r01) && CellRaw(x0 + 1, y0 + 1, r11))
	{
		float twist = std::fabs(r00 + r11 - r10 - r01) * surfZ * surfZ / s_NearPlane;
		if (twist < 0.06f + surfZ * 0.01f)
		{
			float top = r00 + (r10 - r00) * tx;
			float bottom = r01 + (r11 - r01) * tx;
			surfZ = RawToMeters(top + (bottom - top) * ty);
		}
	}
	return true;
}

static bool SurfaceAt(V3 p, float& pointZ, float& surfZ, int& cx, int& cy)
{
	GridHit h;
	if (!GridLookup(p, h)) { return false; }
	if (!SurfaceDepth(h, surfZ)) { return false; }
	cx = h.cx;
	cy = h.cy;
	pointZ = h.pointZ;
	return true;
}

static bool GridNormal(int cx, int cy, V3& out)
{
	float z0;
	if (!CellMid(cx, cy, z0)) { return false; }
	V3 p0 = GridPoint(cx, cy, z0);
	for (int k = 2; k >= 1; k--)
	{
		float zl, zr, zu, zd;
		if (!CellMid(cx - k, cy, zl) || !CellMid(cx + k, cy, zr) || !CellMid(cx, cy - k, zu) || !CellMid(cx, cy + k, zd)) { continue; }
		float lo = std::min(std::min(std::min(zl, zr), std::min(zu, zd)), z0);
		float hi = std::max(std::max(std::max(zl, zr), std::max(zu, zd)), z0);
		if (hi > lo * 3.0f) { continue; }
		V3 pl = GridPoint(cx - k, cy, zl);
		V3 pr = GridPoint(cx + k, cy, zr);
		V3 pu = GridPoint(cx, cy - k, zu);
		V3 pd = GridPoint(cx, cy + k, zd);
		V3 n = Cross(Sub(pr, pl), Sub(pd, pu));
		if (Len(n) < 1e-6f) { continue; }
		n = Norm(n);
		if (Dot(n, Sub(s_GridCam.pos, p0)) < 0.0f) { n = Mul(n, -1.0f); }
		float tol = 0.05f + z0 * 0.02f;
		if (std::fabs(Dot(Sub(pl, p0), n)) > tol || std::fabs(Dot(Sub(pr, p0), n)) > tol ||
			std::fabs(Dot(Sub(pu, p0), n)) > tol || std::fabs(Dot(Sub(pd, p0), n)) > tol) { continue; }
		out = n;
		return true;
	}
	return false;
}

static void UpdateScreenRects()
{
	s_MinimapRect.valid = false;
	const auto& compass = s_Mumble->Context.Compass;
	if (compass.Width > 0 && compass.Height > 0)
	{
		float w = (float)compass.Width;
		float h = (float)compass.Height;
		s_MinimapRect.x1 = s_Cam.width - w;
		s_MinimapRect.x2 = s_Cam.width;
		if (s_Mumble->Context.IsCompassTopRight)
		{
			s_MinimapRect.y1 = 0.0f;
			s_MinimapRect.y2 = h + 1.0f;
		}
		else
		{
			float delta = 37.0f;
			if (s_Identity)
			{
				switch (s_Identity->UISize)
				{
				case Mumble::EUIScale::Small:  delta = 33.0f; break;
				case Mumble::EUIScale::Large:  delta = 41.0f; break;
				case Mumble::EUIScale::Larger: delta = 45.0f; break;
				default: break;
				}
			}
			s_MinimapRect.y1 = s_Cam.height - h - delta;
			s_MinimapRect.y2 = s_Cam.height - delta;
		}
		s_MinimapRect.valid = true;
	}

	float uiScale = 1.0f;
	if (s_Identity)
	{
		switch (s_Identity->UISize)
		{
		case Mumble::EUIScale::Small:  uiScale = 0.9f; break;
		case Mumble::EUIScale::Large:  uiScale = 1.11f; break;
		case Mumble::EUIScale::Larger: uiScale = 1.22f; break;
		default: break;
		}
	}
	float trackerTop = (s_MinimapRect.valid && s_Mumble->Context.IsCompassTopRight) ? s_MinimapRect.y2 : 0.0f;
	s_TrackerRect.x1 = s_Cam.width - 320.0f * uiScale;
	s_TrackerRect.x2 = s_Cam.width;
	s_TrackerRect.y1 = trackerTop;
	s_TrackerRect.y2 = trackerTop + s_Cam.height * 0.45f;
	s_TrackerRect.valid = true;

	s_CharacterRect.valid = false;
	V3 feet = ToV3(s_Mumble->AvatarPosition);
	V3 head = Add(feet, { 0.0f, 2.3f, 0.0f });
	float fx, fy, fz, hx, hy, hz;
	if (Project(feet, fx, fy, fz) && Project(head, hx, hy, hz))
	{
		float pxPerMeter = (s_Cam.height * 0.5f) / (fz * s_Cam.tanHalf);
		float halfW = 0.8f * pxPerMeter;
		s_CharacterRect.x1 = std::min(fx, hx) - halfW;
		s_CharacterRect.x2 = std::max(fx, hx) + halfW;
		s_CharacterRect.y1 = std::min(fy, hy) - 0.2f * pxPerMeter;
		s_CharacterRect.y2 = std::max(fy, hy) + 0.2f * pxPerMeter;
		s_CharacterRect.valid = true;
	}
}

static bool InRect(const ScreenRect& r, float x, float y)
{
	return r.valid && x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2;
}

static bool OnScreen(float sx, float sy)
{
	return sx >= 0.0f && sy >= 0.0f && sx < s_Cam.width && sy < s_Cam.height;
}

static bool ScreenAllowed(float sx, float sy)
{
	if (!OnScreen(sx, sy)) { return false; }
	if (InRect(s_MinimapRect, sx, sy) || InRect(s_TrackerRect, sx, sy)) { return false; }
	if (S.avoidCharacter && InRect(s_CharacterRect, sx, sy)) { return false; }
	return true;
}

static float LineHeightAt(float distance)
{
	float h = S.worldHeight;
	if (S.autoScale && S.refDistance > 0.1f)
	{
		float scale = distance / S.refDistance;
		if (scale < 0.5f) { scale = 0.5f; }
		if (scale > 6.0f) { scale = 6.0f; }
		h *= scale;
	}
	return h;
}

static float TextAspect(const std::string& text)
{
	ImFont* font = FontFor(text);
	if (!font) { return 4.0f; }
	ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text.c_str());
	return ts.y > 0.0f ? ts.x / ts.y : 4.0f;
}

static float ExtrudeOf(float height)
{
	return S.extrude ? S.extrudeDepth * height : 0.0f;
}

struct SpotCheck
{
	float clear = 0.0f;
	int   edges = 0;
	float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
	float centerY = 0.0f;
	float protrude = 0.0f;
};

static bool AirFree(V3 q, float height)
{
	GridHit h;
	if (!GridLookup(q, h)) { return false; }
	float margin = 0.35f + 0.15f * height + h.pointZ * 0.01f;
	return h.nearZ >= h.pointZ + margin;
}

static int BackdropEdges(V3 anchor, V3 right, V3 up, V3 back, float height, float aspect)
{
	float halfH = height * 0.55f;
	float halfW = height * 0.5f * aspect * 1.05f;
	float depth = ExtrudeOf(height);
	float gx1 = 1e9f, gy1 = 1e9f, gx2 = -1e9f, gy2 = -1e9f;
	float textZ = 0.0f;
	for (int c = 0; c < 8; c++)
	{
		V3 q = Add(Add(anchor, Mul(right, ((c & 1) ? 1.0f : -1.0f) * halfW)), Mul(up, ((c & 2) ? 1.0f : -1.0f) * halfH));
		if (c & 4) { q = Add(q, Mul(back, depth)); }
		float sx, sy, z;
		if (!ProjectWith(s_GridCam, q, sx, sy, z)) { return 0; }
		gx1 = std::min(gx1, sx); gx2 = std::max(gx2, sx);
		gy1 = std::min(gy1, sy); gy2 = std::max(gy2, sy);
		textZ = std::max(textZ, z);
	}
	int x1 = std::max(0, (int)std::floor(gx1 / s_GridCam.width * s_GridW));
	int x2 = std::min(s_GridW - 1, (int)std::floor(gx2 / s_GridCam.width * s_GridW));
	int y1 = std::max(0, (int)std::floor(gy1 / s_GridCam.height * s_GridH));
	int y2 = std::min(s_GridH - 1, (int)std::floor(gy2 / s_GridCam.height * s_GridH));
	float closeLimit = textZ * 1.6f + 2.0f;
	int edges = 0;
	for (int cy = y1; cy <= y2; cy++)
	{
		for (int cx = x1; cx <= x2; cx++)
		{
			int i = cy * s_GridW + cx;
			float n = s_GridNear[i];
			float f = s_GridFar[i];
			if (n >= closeLimit) { continue; }
			if (f > 1e8f || f > n * 1.3f + 0.5f) { edges++; }
		}
	}
	return edges;
}

static SpotCheck CheckAirSpot(V3 anchor, V3 right, V3 up, V3 back, float height, float aspect)
{
	SpotCheck rc;
	float halfH = height * 0.5f;
	float halfW = halfH * aspect;
	float depth = ExtrudeOf(height);
	bool grid = GridUsable();
	int total = 0, good = 0;
	for (int i = 0; i < 15; i++)
	{
		for (int j = 0; j < 5; j++)
		{
			float u = (i / 7.0f - 1.0f) * 1.05f;
			float v = (j / 2.0f - 1.0f) * 1.1f;
			V3 q = Add(Add(anchor, Mul(right, u * halfW)), Mul(up, v * halfH));
			total++;
			float sx, sy, z;
			if (!Project(q, sx, sy, z)) { continue; }
			rc.minX = std::min(rc.minX, sx); rc.maxX = std::max(rc.maxX, sx);
			rc.minY = std::min(rc.minY, sy); rc.maxY = std::max(rc.maxY, sy);
			if (!ScreenAllowed(sx, sy)) { continue; }
			if (!grid) { good++; continue; }
			if (!AirFree(q, height)) { continue; }
			if (depth > 0.0f && ((i + j) & 1) == 0 && !AirFree(Add(q, Mul(back, depth)), height)) { continue; }
			good++;
		}
	}
	rc.clear = (float)good / (float)total;
	rc.centerY = (rc.minY + rc.maxY) * 0.5f;
	if (grid) { rc.edges = BackdropEdges(anchor, right, up, back, height, aspect); }
	return rc;
}

static SpotCheck CheckWallSpot(V3 anchor, V3 right, float height, float aspect, V3 wallPoint, V3 normal)
{
	SpotCheck rc;
	float halfH = height * 0.5f;
	float halfW = halfH * aspect;
	int total = 0, good = 0;
	for (int i = 0; i < 15; i++)
	{
		for (int j = 0; j < 5; j++)
		{
			float u = (i / 7.0f - 1.0f) * 1.08f;
			float v = (j / 2.0f - 1.0f) * 1.12f;
			V3 q = Add(Add(anchor, Mul(right, u * halfW)), Mul(WorldUp, v * halfH));
			total++;
			float sx, sy, z;
			if (!Project(q, sx, sy, z)) { continue; }
			rc.minX = std::min(rc.minX, sx); rc.maxX = std::max(rc.maxX, sx);
			rc.minY = std::min(rc.minY, sy); rc.maxY = std::max(rc.maxY, sy);
			if (!ScreenAllowed(sx, sy)) { continue; }
			GridHit h;
			if (!GridLookup(q, h)) { continue; }
			V3 d = Sub(q, s_GridCam.pos);
			float dn = Dot(d, normal);
			if (dn > -1e-4f) { continue; }
			float t = Dot(Sub(wallPoint, s_GridCam.pos), normal) / dn;
			if (t < 1.0f) { continue; }
			float expected = h.pointZ * t;
			float surface;
			if (!SurfaceDepth(h, surface)) { continue; }
			float tol = 0.1f + expected * 0.012f;
			if (std::fabs(surface - expected) > tol) { continue; }
			rc.protrude = std::max(rc.protrude, -dn * (expected - surface) / h.pointZ);
			good++;
		}
	}
	rc.clear = (float)good / (float)total;
	rc.centerY = (rc.minY + rc.maxY) * 0.5f;
	return rc;
}

static bool BlockedFraction(const SpawnedLine& line, float& fraction)
{
	float halfH = line.height * 0.5f;
	float halfW = halfH * TextAspect(line.text);
	int total = 0, considered = 0, blocked = 0;
	for (int i = 0; i < 9; i++)
	{
		for (int j = 0; j < 3; j++)
		{
			float u = (i / 4.0f - 1.0f) * 0.9f;
			float v = (j - 1.0f) * 0.8f;
			V3 q = Add(Add(line.anchor, Mul(line.right, u * halfW)), Mul(line.up, v * halfH));
			total++;
			float sx, sy, z;
			if (!Project(q, sx, sy, z) || !OnScreen(sx, sy)) { continue; }
			if (InRect(s_MinimapRect, sx, sy) || InRect(s_TrackerRect, sx, sy) || InRect(s_CharacterRect, sx, sy)) { continue; }
			GridHit h;
			if (!GridLookup(q, h)) { continue; }
			considered++;
			float margin = 0.4f + 0.3f * line.height + h.pointZ * 0.01f;
			if (h.farZ < h.pointZ - margin) { blocked++; }
		}
	}
	if (considered * 2 < total) { return false; }
	fraction = (float)blocked / (float)considered;
	return true;
}

static bool LineScreenRect(const SpawnedLine& line, float pad, float& x1, float& y1, float& x2, float& y2)
{
	float halfH = line.height * 0.5f;
	float halfW = halfH * TextAspect(line.text);
	float depth = ExtrudeOf(line.height);
	x1 = 1e9f; y1 = 1e9f; x2 = -1e9f; y2 = -1e9f;
	bool any = false;
	for (int c = 0; c < 8; c++)
	{
		float u = (c & 1) ? 1.0f : -1.0f;
		float v = (c & 2) ? 1.0f : -1.0f;
		V3 q = Add(Add(line.anchor, Mul(line.right, u * halfW)), Mul(line.up, v * halfH));
		if (c & 4) { q = Add(q, Mul(line.back, depth)); }
		float sx, sy, z;
		if (Project(q, sx, sy, z))
		{
			any = true;
			x1 = std::min(x1, sx); x2 = std::max(x2, sx);
			y1 = std::min(y1, sy); y2 = std::max(y2, sy);
		}
	}
	if (!any) { return false; }
	float padPx = (y2 - y1) * pad;
	x1 -= padPx; x2 += padPx;
	y1 -= padPx; y2 += padPx;
	return true;
}

static void OverlapAmounts(const SpotCheck& rc, float& active, float& fading)
{
	active = 0.0f;
	fading = 0.0f;
	if (rc.maxX <= rc.minX || rc.maxY <= rc.minY) { return; }
	float area = (rc.maxX - rc.minX) * (rc.maxY - rc.minY);
	for (const auto& line : s_Lines)
	{
		float x1, y1, x2, y2;
		if (!LineScreenRect(line, 0.2f, x1, y1, x2, y2)) { continue; }
		float ix = std::max(0.0f, std::min(x2, rc.maxX) - std::max(x1, rc.minX));
		float iy = std::max(0.0f, std::min(y2, rc.maxY) - std::max(y1, rc.minY));
		float inter = ix * iy;
		if (inter <= 0.0f) { continue; }
		float other = std::max(1.0f, (x2 - x1) * (y2 - y1));
		float amount = std::max(inter / area, inter / other);
		float& slot = line.fadeStart > 0.0 ? fading : active;
		slot = std::max(slot, amount);
	}
}

static float OverlapScore(const SpotCheck& rc, bool& blocked)
{
	float active, fading;
	OverlapAmounts(rc, active, fading);
	blocked = active > 0.01f;
	float penalty = active * 8.0f + fading * 3.0f;
	if (fading > 0.01f) { penalty += 1.0f; }
	return penalty;
}

struct Placement
{
	V3    anchor = { 0, 0, 0 };
	V3    right = { 1, 0, 0 };
	V3    back = { 0, 0, 1 };
	V3    up = { 0, 1, 0 };
	float height = 1.0f;
	bool  aimed = false;
	float clear = 0.0f;
	float score = -1e9f;
	bool  onWall = false;
	bool  close = false;
	bool  continuesSession = false;
	bool  newColumn = false;
	bool  farPick = false;
	V3    wallPoint = { 0, 0, 0 };
};

static float CameraTilt()
{
	if (!S.tiltToCamera) { return 0.0f; }
	float pitch = std::asin(std::max(-1.0f, std::min(1.0f, s_Cam.f.y))) * 180.0f / Pi;
	return Clamp01((std::fabs(pitch) - 25.0f) / 25.0f);
}

static void FaceCamera(Placement& p)
{
	V3 full = Norm(Sub(p.anchor, s_Cam.pos));
	V3 flat = Horizontal(full);
	if (Len(flat) < 0.5f) { flat = Horizontal(s_Cam.f); }
	float t = CameraTilt();
	p.back = t > 0.0f ? Norm(Add(Mul(flat, 1.0f - t), Mul(full, t))) : flat;
	p.right = Cross(WorldUp, p.back);
	p.right = Len(p.right) > 0.05f ? Norm(p.right) : s_Cam.r;
	p.up = Norm(Cross(p.back, p.right));
}

static Placement PickAir(float aspect)
{
	Placement best;
	V3 avatar = ToV3(s_Mumble->AvatarPosition);
	float avatarDepth = std::max(2.0f, Dot(Sub(avatar, s_Cam.pos), s_Cam.f));
	float top = std::min(S.screenTop, S.screenBottom);
	float bottom = std::max(S.screenTop, S.screenBottom);
	float aheadMin = std::min(S.aheadMin, S.aheadMax);
	float aheadMax = std::max(S.aheadMin, S.aheadMax);
	bool mounted = s_Mumble->Context.MountIndex != Mumble::EMountIndex::None;
	float farther = mounted ? S.mountFarther : 1.0f;
	V3 forward = Horizontal(s_Cam.f);
	if (Len(forward) < 0.5f) { forward = Horizontal(ToV3(s_Mumble->AvatarFront)); }
	V3 moveDir = Horizontal(s_MoveVel);
	float speed = Len(V3{ s_MoveVel.x, 0.0f, s_MoveVel.z });
	bool moving = S.followMovement && speed > 2.0f && Len(moveDir) > 0.5f;
	if (moving && S.placeMode == Place_Near) { forward = moveDir; }
	V3 sideways = Norm(Cross(WorldUp, forward));
	bool steep = CameraTilt() >= 0.5f;
	float aimX = 0.0f, aimY = 0.0f, aimZ = 0.0f;
	bool aim = false;
	if (moving && S.placeMode != Place_Near)
	{
		V3 target = Add(Add(avatar, Mul(moveDir, 8.0f + speed * 1.5f)), { 0.0f, 1.5f, 0.0f });
		aim = Project(target, aimX, aimY, aimZ) && aimX > 0.0f && aimX < s_Cam.width && aimY > 0.0f && aimY < s_Cam.height;
	}

	for (int attempt = 0; attempt < 96; attempt++)
	{
		if (attempt == 64 && best.clear >= 0.9f) { break; }
		bool closer = attempt >= 64;
		Placement p;
		if (S.placeMode == Place_Near)
		{
			float side = RandRange(-1.0f, 1.0f) * S.spreadSide;
			float up = RandRange(-1.0f, 1.0f) * S.spreadUp;
			float reach = (mounted && attempt % 2 == 0) ? farther : 1.0f;
			p.anchor = Add(Add(Add(avatar, Mul(forward, S.distance * reach)), Mul(sideways, side)), { 0.0f, S.height + up, 0.0f });
			p.farPick = reach > 1.0f;
		}
		else
		{
			bool aimed = aim && (attempt % 2 == 0);
			float sx = RandRange(S.screenMarginX, 1.0f - S.screenMarginX) * s_Cam.width;
			float sy = RandRange(top, bottom) * s_Cam.height;
			float reach = (mounted && attempt % 2 == 0) ? farther : 1.0f;
			p.farPick = reach > 1.0f;
			float z = std::max(3.0f, avatarDepth + RandRange(aheadMin * reach, std::min(aheadMax * reach, std::max(aheadMax, 60.0f))));
			if (closer) { z = RandRange(std::max(3.0f, avatarDepth * 0.6f), avatarDepth + 8.0f); }
			if (aimed)
			{
				sx = std::max(S.screenMarginX * s_Cam.width, std::min((1.0f - S.screenMarginX) * s_Cam.width, aimX + RandRange(-0.18f, 0.18f) * s_Cam.width));
				sy = std::max(top * s_Cam.height, std::min(bottom * s_Cam.height, aimY + RandRange(-0.15f, 0.08f) * s_Cam.height));
				z = std::max(3.0f, aimZ + RandRange(-2.0f, 4.0f));
			}
			p.aimed = aimed;
			z = std::max(z, 2.0f);
			if (GridUsable())
			{
				GridHit h;
				if (GridLookup(ScreenToWorld(sx, sy, z), h) && h.nearZ < 1e8f)
				{
					float limit = h.nearZ - (1.0f + h.nearZ * 0.03f);
					float floorZ = steep ? avatarDepth + 0.5f : std::max(3.0f, avatarDepth * 0.6f);
					if (z > limit) { z = std::max(floorZ, limit); }
				}
			}
			p.anchor = ScreenToWorld(sx, sy, z);
			if (!steep && p.anchor.y < avatar.y + 0.8f) { p.anchor.y = avatar.y + 0.8f; }
		}
		FaceCamera(p);
		bool shrunk = attempt >= 40 && !closer;
		p.height = LineHeightAt(Len(Sub(p.anchor, s_Cam.pos))) * (shrunk ? 0.7f : 1.0f);
		SpotCheck rc = CheckAirSpot(p.anchor, p.right, p.up, p.back, p.height, aspect);
		p.clear = rc.clear;
		bool overlapsActive = false;
		float overlap = OverlapScore(rc, overlapsActive);
		float hiddenPenalty = rc.clear < 0.9f ? (0.9f - rc.clear) * 25.0f : 0.0f;
		float edgePenalty = rc.edges > 3 ? 4.0f + std::min(4.0f, rc.edges * 0.05f) : 0.0f;
		p.score = rc.clear * 10.0f - hiddenPenalty - edgePenalty - overlap - (overlapsActive ? 30.0f : 0.0f) - (shrunk ? 1.0f : 0.0f) + (p.aimed ? 1.5f : 0.0f) + (p.farPick && rc.clear >= 0.9f ? 1.0f : 0.0f) + Rand01() * 0.3f;
		if (p.score > best.score) { best = p; }
	}
	return best;
}

static bool WallAt(int cx, int cy, V3& outPoint, V3& outNormal)
{
	const int k = 3;
	float z0;
	if (!CellMid(cx, cy, z0) || z0 < 3.0f || z0 > 60.0f) { return false; }
	V3 pts[5][5];
	for (int j = 0; j < 5; j++)
	{
		for (int i = 0; i < 5; i++)
		{
			int x = cx + (i - 2) * k;
			int y = cy + (j - 2) * k;
			float z;
			if (!CellMid(x, y, z) || std::fabs(z - z0) > z0 * 0.3f) { return false; }
			pts[j][i] = GridPoint(x, y, z);
		}
	}
	V3 p0 = pts[2][2];
	V3 tx = { 0, 0, 0 };
	V3 ty = { 0, 0, 0 };
	for (int a = 0; a < 5; a++)
	{
		tx = Add(tx, Sub(pts[a][4], pts[a][0]));
		ty = Add(ty, Sub(pts[4][a], pts[0][a]));
	}
	V3 n = Cross(tx, ty);
	if (Len(n) < 1e-6f) { return false; }
	n = Norm(n);
	if (Dot(n, Sub(s_GridCam.pos, p0)) < 0.0f) { n = Mul(n, -1.0f); }
	if (std::fabs(n.y) > 0.25f) { return false; }

	float tol = 0.06f + z0 * 0.012f;
	for (int j = 0; j < 5; j++)
	{
		for (int i = 0; i < 5; i++)
		{
			if (std::fabs(Dot(Sub(pts[j][i], p0), n)) > tol) { return false; }
		}
	}

	V3 nh = Horizontal(n);
	if (Dot(nh, Norm(Sub(s_Cam.pos, p0))) < 0.5f) { return false; }
	outPoint = p0;
	outNormal = nh;
	return true;
}

static bool FitOnWall(Placement& p, V3 point, V3 normal, float aspect, float baseHeight, int maxTries = 4)
{
	p.back = Mul(normal, -1.0f);
	p.right = Norm(Cross(WorldUp, p.back));
	p.up = WorldUp;
	p.onWall = true;
	p.wallPoint = point;
	float h = baseHeight;
	for (int tries = 0; tries < maxTries; tries++)
	{
		float raised = 0.1f + ExtrudeOf(h);
		p.anchor = Add(point, Mul(normal, raised));
		p.height = h;
		SpotCheck rc = CheckWallSpot(p.anchor, p.right, h, aspect, point, normal);
		bool overlapsActive = false;
		float overlap = rc.clear >= 0.9f ? OverlapScore(rc, overlapsActive) : 0.0f;
		if (rc.clear >= 0.9f && !overlapsActive)
		{
			p.anchor = Add(point, Mul(normal, raised + rc.protrude));
			p.clear = rc.clear;
			float upper = 1.0f - Clamp01(rc.centerY / s_Cam.height);
			p.score = rc.clear * 10.0f - overlap + upper * 1.5f + Rand01() * 0.3f;
			return true;
		}
		h *= 0.8f;
	}
	return false;
}

static Placement PickWall(float aspect, double now)
{
	Placement best;
	if (!GridUsable() || !S.useWalls || S.pinToScreen) { return best; }

	if (s_Wall.active && now - s_Wall.lastTime < 8.0)
	{
		Placement p;
		V3 below = Add(s_Wall.lastPoint, Mul(WorldUp, -(s_Wall.lastHeight * 0.5f + s_Wall.baseHeight * 0.95f)));
		if (FitOnWall(p, below, s_Wall.normal, aspect, s_Wall.baseHeight, 2))
		{
			p.continuesSession = true;
			p.score += 5.0f;
			return p;
		}
		if (s_Wall.columns < 2)
		{
			V3 right = Norm(Cross(WorldUp, Mul(s_Wall.normal, -1.0f)));
			float nextWidth = s_Wall.baseHeight * aspect;
			V3 side = Add(s_Wall.columnTop, Mul(right, s_Wall.columnWidth * 0.5f + nextWidth * 0.5f + s_Wall.baseHeight));
			Placement q;
			if (FitOnWall(q, side, s_Wall.normal, aspect, s_Wall.baseHeight, 2))
			{
				q.continuesSession = true;
				q.newColumn = true;
				q.score += 4.0f;
				return q;
			}
		}
	}

	int marginX = (int)(S.screenMarginX * s_GridW * 0.5f);
	int spanX = std::max(1, s_GridW - 2 * marginX);
	for (int attempt = 0; attempt < 120; attempt++)
	{
		int cx = marginX + (int)(Rand01() * spanX);
		int cy = 3 + (int)(Rand01() * (s_GridH * 0.78f));
		V3 point, normal;
		if (!WallAt(cx, cy, point, normal)) { continue; }
		float sx, sy, z;
		if (!Project(point, sx, sy, z) || !ScreenAllowed(sx, sy)) { continue; }
		Placement p;
		float baseHeight = LineHeightAt(Len(Sub(point, s_Cam.pos)));
		if (!FitOnWall(p, point, normal, aspect, baseHeight)) { continue; }
		if (p.score > best.score) { best = p; }
	}
	return best;
}

static void BuildLayout(SpawnedLine& line, ImFont* font)
{
	line.glyphs.clear();
	line.lift.clear();
	line.liftTarget.clear();
	line.layoutFont = font;
	if (!font) { return; }
	ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, line.text.c_str());
	if (ts.y <= 0.0f) { return; }
	float mpu = line.height / ts.y;
	float halfW = ts.x * 0.5f;
	float halfH = ts.y * 0.5f;
	const char* s = line.text.c_str();
	const char* end = s + line.text.size();
	float x = 0.0f;
	while (s < end)
	{
		unsigned int c = (unsigned int)(unsigned char)*s;
		if (c < 0x80) { s += 1; }
		else
		{
			s += ImTextCharFromUtf8(&c, s, end);
			if (c == 0) { break; }
		}
		if (c == '\n') { x = 0.0f; continue; }
		if (c == '\r') { continue; }
		const ImFontGlyph* g = font->FindGlyph((ImWchar)c);
		if (!g) { continue; }
		if (g->Visible)
		{
			GlyphBox b;
			b.lx = (x + (g->X0 + g->X1) * 0.5f - halfW) * mpu;
			b.ly = (halfH - (g->Y0 + g->Y1) * 0.5f) * mpu;
			b.hw = (g->X1 - g->X0) * 0.5f * mpu;
			b.hh = (g->Y1 - g->Y0) * 0.5f * mpu;
			line.glyphs.push_back(b);
		}
		x += g->AdvanceX;
	}
	line.lift.assign(line.glyphs.size(), V3{ 0, 0, 0 });
	line.liftTarget.assign(line.glyphs.size(), V3{ 0, 0, 0 });
}

static bool LiftStep(SpawnedLine& line)
{
	if (line.liftTarget.size() != line.glyphs.size())
	{
		line.liftTarget.assign(line.glyphs.size(), V3{ 0, 0, 0 });
		line.lift.assign(line.glyphs.size(), V3{ 0, 0, 0 });
	}
	bool changed = false;
	float depth = ExtrudeOf(line.height);
	float limit = 0.15f + 0.35f * line.height;
	float cap = 0.3f + 1.2f * line.height;
	for (size_t g = 0; g < line.glyphs.size(); g++)
	{
		const GlyphBox& box = line.glyphs[g];
		V3& target = line.liftTarget[g];
		V3 base = Add(Add(Add(line.anchor, Mul(line.right, box.lx)), Mul(line.up, box.ly)), target);
		V3 halfRight = Mul(line.right, box.hw);
		V3 halfUp = Mul(line.up, box.hh);
		V3 points[8];
		int count = 0;
		points[count++] = base;
		points[count++] = Add(base, Mul(halfUp, 0.85f));
		points[count++] = Add(base, Mul(halfUp, -0.85f));
		points[count++] = Add(base, Mul(halfRight, 0.8f));
		points[count++] = Add(base, Mul(halfRight, -0.8f));
		if (depth > 0.0f)
		{
			V3 b = Add(base, Mul(line.back, depth));
			points[count++] = b;
			points[count++] = Add(b, Mul(halfUp, 0.85f));
			points[count++] = Add(b, Mul(halfUp, -0.85f));
		}

		V3 bestMove = { 0, 0, 0 };
		float bestLen = 0.0f;
		for (int i = 0; i < count; i++)
		{
			float pointZ, surfZ;
			int cx, cy;
			float sx, sy, sz;
			if (Project(points[i], sx, sy, sz) && InRect(s_CharacterRect, sx, sy)) { continue; }
			if (!SurfaceAt(points[i], pointZ, surfZ, cx, cy)) { continue; }
			float pen = pointZ - surfZ;
			if (pen <= 0.03f + pointZ * 0.006f) { continue; }
			V3 ray = Norm(Sub(points[i], s_GridCam.pos));
			float alongRay = pen / std::max(0.05f, Dot(ray, s_GridCam.f));
			V3 n;
			if (!GridNormal(cx, cy, n)) { n = WorldUp; }
			float inside = alongRay * std::max(0.05f, std::fabs(Dot(ray, n)));
			bool ground = n.y > 0.6f;
			if (inside > (ground ? limit : 0.1f + 0.1f * line.height)) { continue; }
			V3 foot = Add(points[i], Mul(n, inside));
			GridHit fh;
			float footSurf;
			if (!GridLookup(foot, fh) || !SurfaceDepth(fh, footSurf)) { continue; }
			if (std::fabs(footSurf - fh.pointZ) > 0.08f + fh.pointZ * 0.01f) { continue; }
			float need = inside + 0.05f + pointZ * 0.004f;
			V3 move = n.y > 0.6f ? Mul(WorldUp, need / n.y) : Mul(Norm(n), need);
			float len = Len(move);
			if (len > bestLen) { bestLen = len; bestMove = move; }
		}
		if (bestLen > 0.0f)
		{
			V3 next = Add(target, bestMove);
			if (Len(next) <= cap)
			{
				target = next;
				changed = true;
			}
		}
	}
	return changed;
}

static void SettleLetters(SpawnedLine& line)
{
	if (!S.liftLetters) { line.settled = true; return; }
	if (!s_GridReady) { return; }
	for (int i = 0; i < 8; i++)
	{
		if (!LiftStep(line)) { break; }
	}
	line.lift = line.liftTarget;
	line.settled = true;
}

static void SpawnLine(const std::string& text, double now)
{
	float aspect = TextAspect(text);
	Placement air = PickAir(aspect);
	Placement wall = PickWall(aspect, now);

	Placement chosen = air;
	if (wall.onWall)
	{
		bool airPerfect = air.clear >= 0.97f;
		if (wall.continuesSession || !airPerfect || Rand01() < S.wallChance)
		{
			chosen = wall;
		}
	}

	if (chosen.onWall)
	{
		if (!chosen.continuesSession)
		{
			s_Wall.active = true;
			s_Wall.count = 0;
			s_Wall.columns = 0;
			s_Wall.columnTop = chosen.wallPoint;
			s_Wall.columnWidth = 0.0f;
			s_Wall.baseHeight = chosen.height;
		}
		else if (chosen.newColumn)
		{
			s_Wall.columns++;
			s_Wall.columnTop = chosen.wallPoint;
			s_Wall.columnWidth = 0.0f;
		}
		s_Wall.normal = Mul(chosen.back, -1.0f);
		s_Wall.lastPoint = chosen.wallPoint;
		s_Wall.lastHeight = chosen.height;
		s_Wall.columnWidth = std::max(s_Wall.columnWidth, chosen.height * aspect);
		s_Wall.lastTime = now;
		s_Wall.count++;
		if (s_Wall.count >= 8) { s_Wall.active = false; }
	}
	else
	{
		s_Wall.active = false;
	}

	s_LastSpawnClear = (int)(chosen.clear * 100.0f + 0.5f);
	s_LastSpawnWall = chosen.onWall ? 1 : 0;
	if (chosen.score < -1e8f)
	{
		V3 avatar = ToV3(s_Mumble->AvatarPosition);
		V3 forward = Horizontal(s_Cam.f);
		chosen.anchor = Add(Add(avatar, Mul(forward, 5.0f)), { 0.0f, 2.5f, 0.0f });
		FaceCamera(chosen);
		chosen.height = LineHeightAt(Len(Sub(chosen.anchor, s_Cam.pos)));
		chosen.close = true;
	}

	SpawnedLine line;
	line.text = text;
	line.anchor = chosen.anchor;
	line.back = chosen.back;
	line.right = chosen.right;
	line.up = chosen.up;
	line.height = chosen.height;
	line.onWall = chosen.onWall;
	line.start = now;
	line.fadeStart = 0.0;
	line.seed = (unsigned)(Rand01() * 16777215.0f);
	line.id = s_NextLineId++;
	line.kind = chosen.onWall ? Spawn_Wall : (chosen.close ? Spawn_Close : Spawn_Air);
	line.spawnClear = s_LastSpawnClear;
	BuildLayout(line, FontFor(line.text));
	line.settleDeadline = now + 0.2;
	if (S.pinToScreen)
	{
		V3 rel = Sub(line.anchor, s_Cam.pos);
		line.pinned = true;
		line.camAnchor = { Dot(rel, s_Cam.r), Dot(rel, s_Cam.u), Dot(rel, s_Cam.f) };
		line.camRight = { Dot(line.right, s_Cam.r), Dot(line.right, s_Cam.u), Dot(line.right, s_Cam.f) };
		line.camUp = { Dot(line.up, s_Cam.r), Dot(line.up, s_Cam.u), Dot(line.up, s_Cam.f) };
		line.camBack = { Dot(line.back, s_Cam.r), Dot(line.back, s_Cam.u), Dot(line.back, s_Cam.f) };
		line.settled = true;
	}
	else
	{
		SettleLetters(line);
	}

	s_Lines.push_back(line);
	while (s_Lines.size() > 24)
	{
		s_Lines.erase(s_Lines.begin());
	}
}

struct AnimState
{
	float    elapsed;
	float    disappear;
	unsigned seed;
	unsigned lineId = 0;
	const std::vector<V3>* lift = nullptr;
	V3  front = { 0, 0, 0 };
	int pass = 0;
	float beat = 0.0f;
	int   glyphCount = 0;
	float progress = -1.0f;
	const std::vector<float>* wordFrac = nullptr;
	float litGlyphs = -1.0f;
	ImU32 litColor = 0;
	bool  karaokeLayer = false;
	Color4 textColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	Color4 depthColor = { 0.59f, 0.59f, 0.69f, 1.0f };
	bool  chorusGlow = false;
	Color4 glowColor = { 1.0f, 0.8f, 0.35f, 0.6f };
};

struct GlyphXform
{
	float alpha = 1.0f;
	float scale = 1.0f;
	float angle = 0.0f;
	float dx    = 0.0f;
	float dy    = 0.0f;
	float dz    = 0.0f;
	float land  = -1.0f;
	float flash = 0.0f;
};

static GlyphXform GlyphAnim(const AnimState& a, int glyph)
{
	GlyphXform g;
	bool perLetter = S.appearMode != Appear_Fade && S.appearMode != Appear_FadeRise && S.appearMode != Appear_Zoom;
	float delay = a.glyphCount > 0 ? std::min(S.letterDelay, 0.35f / (float)a.glyphCount) : S.letterDelay;
	float local = a.elapsed - (perLetter ? glyph * delay : 0.0f);
	float p = Clamp01(local / S.appearTime);
	float e = EaseOut(p);
	unsigned key = a.seed + (unsigned)glyph * 7919u;

	switch (S.appearMode)
	{
	case Appear_Fade:
		g.alpha = p;
		break;
	case Appear_FadeRise:
		g.alpha = p;
		g.dy = -(1.0f - e) * 0.35f;
		break;
	case Appear_Letters:
		g.alpha = p;
		g.dy = -(1.0f - e) * 0.25f;
		g.scale = 0.5f + 0.5f * e;
		break;
	case Appear_Scatter:
		g.alpha = p;
		g.dx = (Hash01(key, 1) - 0.5f) * 4.0f * (1.0f - e);
		g.dy = (Hash01(key, 2) - 0.5f) * 3.0f * (1.0f - e);
		g.dz = (Hash01(key, 3) - 0.5f) * 3.0f * (1.0f - e);
		break;
	case Appear_Bounce:
		g.alpha = Clamp01(p * 3.0f);
		g.dy = (1.0f - EaseOutBounce(p)) * 1.2f;
		break;
	case Appear_Spin:
		g.alpha = p;
		g.angle = (1.0f - e) * 2.0f * Pi * (Hash01(key, 4) > 0.5f ? 1.0f : -1.0f);
		g.scale = 0.3f + 0.7f * e;
		break;
	case Appear_Pop:
		g.alpha = Clamp01(p * 2.0f);
		g.scale = std::max(0.0f, EaseOutBack(p));
		break;
	case Appear_Drop:
		g.alpha = Clamp01(p * 2.5f);
		g.dy = (1.0f - EaseOutBounce(p)) * 2.5f;
		g.angle = (1.0f - e) * 0.4f * (Hash01(key, 8) - 0.5f);
		break;
	case Appear_Zoom:
		g.alpha = p;
		g.scale = 1.0f + (1.0f - e) * 2.0f;
		g.dz = -(1.0f - e) * 1.5f;
		break;
	case Appear_Typewriter:
		g.alpha = local >= 0.0f ? 1.0f : 0.0f;
		g.scale = local >= 0.0f ? 1.0f + 0.25f * (1.0f - Clamp01(local / 0.08f)) : 1.0f;
		break;
	case Appear_WaveIn:
		g.alpha = p;
		g.dy = -(1.0f - e) * 0.8f;
		g.angle = (1.0f - e) * 0.9f;
		break;
	case Appear_Glitch:
	{
		g.alpha = p < 1.0f ? (Hash01(key + (unsigned)(a.elapsed * 30.0f), 9) > 0.35f ? p : 0.15f) : 1.0f;
		float jitter = (1.0f - e) * 0.35f;
		unsigned tick = (unsigned)(a.elapsed * 24.0f);
		g.dx = (Hash01(key + tick, 10) - 0.5f) * jitter;
		g.dy = (Hash01(key + tick, 11) - 0.5f) * jitter * 0.5f;
		break;
	}
	case Appear_FromBehind:
		g.alpha = p;
		g.dz = (1.0f - e) * 3.0f;
		g.scale = 0.6f + 0.4f * e;
		break;
	case Appear_SlideIn:
		g.alpha = p;
		g.dx = -(1.0f - e) * 2.5f;
		break;
	case Appear_Flip:
	{
		g.alpha = Clamp01(p * 2.0f);
		float flip = (1.0f - e) * Pi * 0.5f;
		g.scale = std::max(0.05f, std::cos(flip));
		g.dy = std::sin(flip) * 0.3f;
		break;
	}
	case Appear_Elastic:
		g.alpha = Clamp01(p * 3.0f);
		g.scale = p >= 1.0f ? 1.0f : std::max(0.0f, 1.0f - std::pow(2.0f, -9.0f * p) * std::cos(p * 5.0f * Pi));
		break;
	case Appear_RainDown:
		g.alpha = Clamp01(p * 4.0f);
		g.dy = (1.0f - EaseOutBounce(p)) * 6.0f;
		g.dz = -(1.0f - e) * 0.5f;
		break;
	case Appear_Swirl:
	{
		g.alpha = p;
		float ang = (1.0f - e) * 3.0f * Pi + (float)glyph * 0.4f;
		float radius = (1.0f - e) * 2.0f;
		g.dx = std::cos(ang) * radius;
		g.dy = std::sin(ang) * radius;
		g.angle = (1.0f - e) * 2.0f * Pi;
		g.scale = 0.4f + 0.6f * e;
		break;
	}
	}

	float t = a.elapsed;
	switch (S.idleMode)
	{
	case Idle_Float:
		g.dy += std::sin(t * 1.6f + (float)(a.seed % 628) * 0.01f) * 0.06f * S.idleStrength;
		break;
	case Idle_Wave:
		g.dy += std::sin(t * 4.0f - glyph * 0.55f) * 0.08f * S.idleStrength;
		break;
	case Idle_Pulse:
		g.scale *= 1.0f + 0.05f * S.idleStrength * std::sin(t * 5.0f);
		break;
	case Idle_Shake:
	{
		unsigned tick = (unsigned)(t * 20.0f);
		g.dx += (Hash01(key + tick, 12) - 0.5f) * 0.04f * S.idleStrength;
		g.dy += (Hash01(key + tick, 13) - 0.5f) * 0.04f * S.idleStrength;
		break;
	}
	case Idle_Swing:
		g.angle += std::sin(t * 2.2f + glyph * 0.3f) * 0.12f * S.idleStrength;
		break;
	case Idle_Jelly:
		g.scale *= 1.0f + 0.07f * S.idleStrength * std::sin(t * 6.0f - glyph * 0.8f);
		g.dy += std::sin(t * 6.0f - glyph * 0.8f) * 0.03f * S.idleStrength;
		break;
	case Idle_Glitch:
	{
		unsigned tick = (unsigned)(t * 12.0f);
		if (Hash01(a.seed + tick, 14) > 0.92f)
		{
			g.dx += (Hash01(key + tick, 15) - 0.5f) * 0.2f * S.idleStrength;
			g.alpha *= Hash01(key + tick, 16) > 0.5f ? 1.0f : 0.4f;
		}
		break;
	}
	}

	if (S.landEffect != Land_None && a.disappear <= 0.0f)
	{
		float landAt = (perLetter ? glyph * delay : 0.0f) + (S.appearMode == Appear_Typewriter ? 0.0f : S.appearTime);
		float lt = (a.elapsed - landAt) / 0.45f;
		if (lt >= 0.0f && lt < 1.0f)
		{
			g.land = lt;
			if (S.landEffect == Land_Bounce) { g.scale *= 1.0f + 0.35f * std::sin(lt * Pi) * (1.0f - lt); g.dy += 0.12f * std::sin(lt * Pi) * (1.0f - lt); }
			if (S.landEffect == Land_Flash) { g.flash = 1.0f - lt; g.scale *= 1.0f + 0.12f * (1.0f - lt); }
		}
	}

	float d = a.disappear;
	if (S.disappearMode == Disappear_Letters && a.glyphCount > 0)
	{
		float spread = 0.6f;
		float startAt = spread * (float)glyph / (float)std::max(1, a.glyphCount);
		d = Clamp01((d - startAt) / (1.0f - spread));
	}
	float de = EaseOut(d);
	g.alpha *= 1.0f - d;
	switch (S.disappearMode)
	{
	case Disappear_FadeUp:
		g.dy += de * 0.4f;
		break;
	case Disappear_Scatter:
		g.dx += (Hash01(key, 5) - 0.5f) * 4.0f * de;
		g.dy += (Hash01(key, 6) - 0.2f) * 3.0f * de;
		g.dz += (Hash01(key, 7) - 0.5f) * 3.0f * de;
		break;
	case Disappear_Shrink:
		g.scale *= 1.0f - de;
		break;
	case Disappear_Drop:
		g.dy -= d * d * 3.0f;
		g.angle += d * (Hash01(key, 17) - 0.5f) * 2.0f;
		break;
	case Disappear_SpinOut:
		g.angle += de * 2.0f * Pi * (Hash01(key, 18) > 0.5f ? 1.0f : -1.0f);
		g.scale *= 1.0f - de * 0.8f;
		break;
	case Disappear_BlowAway:
		g.dx += de * (2.5f + Hash01(key, 19) * 2.0f);
		g.dy += de * (Hash01(key, 20) - 0.3f) * 1.2f;
		g.angle += de * (Hash01(key, 21) - 0.5f) * 3.0f;
		break;
	case Disappear_Burst:
		g.scale *= 1.0f + de * 1.5f;
		break;
	case Disappear_Letters:
		g.dy += de * 0.3f;
		break;
	case Disappear_Sink:
		g.dy -= de * 0.6f;
		g.dz += de * 0.8f;
		break;
	case Disappear_SlideOut:
		g.dx += de * 2.5f;
		break;
	case Disappear_FlipOut:
		g.scale *= std::max(0.05f, std::cos(de * Pi * 0.5f));
		g.dy += std::sin(de * Pi * 0.5f) * 0.3f;
		break;
	case Disappear_Melt:
	{
		float drip = Clamp01(d * 1.4f - Hash01(key, 22) * 0.4f);
		g.dy -= drip * drip * (1.2f + Hash01(key, 23) * 1.5f);
		g.scale *= 1.0f - drip * 0.3f;
		break;
	}
	case Disappear_Shatter:
		g.dx += (Hash01(key, 24) - 0.5f) * 3.0f * de;
		g.dy += (Hash01(key, 25) * 1.2f) * de - d * d * 2.5f;
		g.dz += (Hash01(key, 26) - 0.5f) * 2.0f * de;
		g.angle += (Hash01(key, 27) - 0.5f) * 8.0f * de;
		g.scale *= 1.0f - de * 0.4f;
		break;
	case Disappear_SwirlOut:
	{
		float ang = de * 3.0f * Pi + (float)glyph * 0.4f;
		g.dx += std::cos(ang) * de * 2.0f;
		g.dy += std::sin(ang) * de * 2.0f + de * 0.5f;
		g.angle += de * 2.0f * Pi;
		g.scale *= 1.0f - de * 0.6f;
		break;
	}
	}
	return g;
}

struct TextFrame
{
	ImFont* font;
	float   localSize;
	float   halfW;
	float   halfH;
	float   metersPerUnit;
	float   height;
	V3      anchor;
	V3      right;
	V3      back;
	V3      up;
};

static V3 LocalToWorld(const TextFrame& f, float lx, float ly)
{
	return Add(Add(f.anchor, Mul(f.right, (lx - f.halfW) * f.metersPerUnit)), Mul(f.up, (f.halfH - ly) * f.metersPerUnit));
}

static ImU32 ToU32(const Color4& c, float alphaMul = 1.0f)
{
	return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], Clamp01(c[3] * alphaMul)));
}

static float OffPlaneWeight(V3 lift, V3 front)
{
	return Clamp01(Dot(lift, front) / 0.06f);
}

static float SeedRand(unsigned& state);

static void EmitLayer(ImDrawList* dl, const TextFrame& f, const char* text, ImU32 color, V3 worldOffset, float sdx, float sdy, const AnimState& a)
{
	int start = dl->VtxBuffer.Size;
	bool ownAtlas = f.font && f.font->ContainerAtlas && f.font->ContainerAtlas->TexID != dl->_CmdHeader.TextureId;
	if (ownAtlas) { dl->PushTextureID(f.font->ContainerAtlas->TexID); }
	dl->AddText(f.font, f.localSize, { 0.0f, 0.0f }, color, text);
	if (ownAtlas) { dl->PopTextureID(); }
	int end = dl->VtxBuffer.Size;
	struct LandMark { ImVec2 c; float size; float t; float alpha; unsigned seed; };
	std::vector<LandMark> marks;
	bool landLayer = a.karaokeLayer && a.pass != 2 && (S.landEffect == Land_Ring || S.landEffect == Land_Sparks);

	for (int q = start; q + 3 < end; q += 4)
	{
		int glyph = (q - start) / 4;
		GlyphXform g = GlyphAnim(a, glyph);
		g.scale *= 1.0f + a.beat;

		V3 lift = (a.lift && glyph < (int)a.lift->size()) ? (*a.lift)[glyph] : V3{ 0, 0, 0 };
		if (a.pass != 0)
		{
			float weight = OffPlaneWeight(lift, a.front);
			g.alpha *= a.pass == 1 ? 1.0f - weight : weight;
		}

		float ca = std::cos(g.angle);
		float sa = std::sin(g.angle);
		V3 glyphOffset = Add(Add(Add(Add(worldOffset, Mul(f.right, g.dx * f.height)), Mul(f.up, g.dy * f.height)), Mul(f.back, g.dz * f.height)), lift);

		ImDrawVert* v = &dl->VtxBuffer.Data[q];
		float cx = (v[0].pos.x + v[1].pos.x + v[2].pos.x + v[3].pos.x) * 0.25f;
		float cy = (v[0].pos.y + v[1].pos.y + v[2].pos.y + v[3].pos.y) * 0.25f;

		float px[4], py[4];
		bool visible = true;
		for (int k = 0; k < 4; k++)
		{
			float ox = v[k].pos.x - cx;
			float oy = v[k].pos.y - cy;
			float rx = (ox * ca - oy * sa) * g.scale;
			float ry = (ox * sa + oy * ca) * g.scale;
			V3 world = Add(LocalToWorld(f, cx + rx, cy + ry), glyphOffset);
			float pd;
			if (!Project(world, px[k], py[k], pd)) { visible = false; }
		}

		float alphaMul = visible ? Clamp01(g.alpha) : 0.0f;
		if (a.wordFrac && a.progress >= 0.0f && glyph < (int)a.wordFrac->size())
		{
			alphaMul *= Clamp01((a.progress - (*a.wordFrac)[glyph]) / 0.04f + 0.05f);
		}
		float lit = (a.karaokeLayer && a.litGlyphs >= 0.0f) ? Clamp01(a.litGlyphs - (float)glyph) : 0.0f;
		if (landLayer && visible && g.land >= 0.0f && alphaMul > 0.05f)
		{
			float gx = (px[0] + px[1] + px[2] + px[3]) * 0.25f;
			float gy = (py[0] + py[1] + py[2] + py[3]) * 0.25f;
			float size = std::sqrt((px[2] - px[0]) * (px[2] - px[0]) + (py[2] - py[0]) * (py[2] - py[0]));
			marks.push_back({ ImVec2(gx, gy), size, g.land, alphaMul, a.seed + (unsigned)glyph * 131u });
		}
		float whiten = a.karaokeLayer ? g.flash * 0.85f : 0.0f;
		for (int k = 0; k < 4; k++)
		{
			if (whiten > 0.0f)
			{
				ImU32 c0 = v[k].col;
				ImU32 out = c0 & IM_COL32_A_MASK;
				for (int shift = 0; shift < 24; shift += 8)
				{
					float from = (float)((c0 >> shift) & 0xFF);
					out |= ((ImU32)(from + (255.0f - from) * whiten + 0.5f) & 0xFF) << shift;
				}
				v[k].col = out;
			}
			if (lit > 0.0f)
			{
				ImU32 c0 = v[k].col;
				ImU32 out = c0 & IM_COL32_A_MASK;
				for (int shift = 0; shift < 24; shift += 8)
				{
					float from = (float)((c0 >> shift) & 0xFF);
					float to = (float)((a.litColor >> shift) & 0xFF);
					out |= ((ImU32)(from + (to - from) * lit + 0.5f) & 0xFF) << shift;
				}
				v[k].col = out;
			}
			if (visible)
			{
				v[k].pos.x = px[k] + sdx;
				v[k].pos.y = py[k] + sdy;
			}
			else
			{
				v[k].pos.x = -10000.0f;
				v[k].pos.y = -10000.0f;
			}
			ImU32 c = v[k].col;
			ImU32 alpha = (ImU32)(((c >> IM_COL32_A_SHIFT) & 0xFF) * alphaMul);
			v[k].col = (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
		}
	}

	for (const auto& m : marks)
	{
		float fade = (1.0f - m.t) * m.alpha;
		ImU32 col = ToU32({ std::min(1.0f, a.textColor[0] * 0.5f + 0.5f), std::min(1.0f, a.textColor[1] * 0.5f + 0.5f), std::min(1.0f, a.textColor[2] * 0.5f + 0.5f), 1.0f }, fade);
		if (S.landEffect == Land_Ring)
		{
			dl->AddCircle(m.c, m.size * (0.35f + 0.9f * m.t), col, 24, std::max(1.0f, m.size * 0.07f * (1.0f - m.t)));
		}
		else
		{
			unsigned st = m.seed;
			for (int k = 0; k < 6; k++)
			{
				float ang = k * Pi / 3.0f + SeedRand(st) * 0.8f;
				float reach = m.size * (0.25f + 0.85f * m.t) * (0.7f + SeedRand(st) * 0.5f);
				ImVec2 pt = { m.c.x + std::cos(ang) * reach, m.c.y + std::sin(ang) * reach };
				dl->AddCircleFilled(pt, std::max(1.0f, m.size * 0.05f * (1.0f - m.t)), col, 6);
			}
		}
	}
}

static void EmitRing(ImDrawList* dl, const TextFrame& f, const char* text, ImU32 color, float radius, const AnimState& a)
{
	static const float dirs[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 0.707f, 0.707f }, { -0.707f, 0.707f }, { 0.707f, -0.707f }, { -0.707f, -0.707f } };
	for (int i = 0; i < 8; i++)
	{
		EmitLayer(dl, f, text, color, { 0, 0, 0 }, dirs[i][0] * radius, dirs[i][1] * radius, a);
	}
}

static void EmitFrontFace(ImDrawList* dl, const TextFrame& f, const char* text, float pixelHeight, const AnimState& a)
{
	if (S.glow || a.chorusGlow)
	{
		float size = std::max(2.0f, pixelHeight * (S.glow ? S.glowSize : 0.15f) * (a.chorusGlow ? 1.4f : 1.0f));
		for (int k = 3; k >= 1; k--)
		{
			float alphaMul = 0.35f * (1.0f - (k - 1) / 3.0f);
			EmitRing(dl, f, text, ToU32(a.glowColor, alphaMul), size * k / 3.0f, a);
		}
	}
	if (S.outline)
	{
		float width = pixelHeight * S.outlineWidth;
		if (width < 1.0f) { width = 1.0f; }
		if (width > 6.0f) { width = 6.0f; }
		EmitRing(dl, f, text, ToU32(S.outlineColor), width, a);
	}
	AnimState face = a;
	face.karaokeLayer = true;
	EmitLayer(dl, f, text, ToU32(a.textColor), { 0, 0, 0 }, 0.0f, 0.0f, face);
}

static void DrawCover(ImDrawList* dl, ImFont* font, const SpawnedLine& line, const AnimState& a)
{
	V3 right = line.right;
	if (S.readableBack && Dot(s_Cam.r, right) < 0.0f) { right = Mul(right, -1.0f); }
	float localSize = font->FontSize;
	ImVec2 ts = font->CalcTextSizeA(localSize, FLT_MAX, 0.0f, line.text.c_str());
	if (ts.x > s_Cam.width - 8.0f)
	{
		localSize *= (s_Cam.width - 8.0f) / ts.x;
		ts = font->CalcTextSizeA(localSize, FLT_MAX, 0.0f, line.text.c_str());
	}
	if (ts.y <= 0.0f) { return; }
	float metersPerUnit = line.height / ts.y;
	float halfW = ts.x * 0.5f * metersPerUnit;
	float pad = font->CalcTextSizeA(localSize, FLT_MAX, 0.0f, "       ").x * metersPerUnit;
	float size = std::min(line.height * 1.3f, pad * 0.9f);
	V3 center = Add(line.anchor, Mul(right, -halfW + pad * 0.45f));
	V3 r = Mul(right, size * 0.5f);
	V3 u = Mul(line.up, size * 0.5f);
	V3 corners[4] = { Add(Sub(center, r), u), Add(Add(center, r), u), Sub(Add(center, r), u), Sub(Sub(center, r), u) };
	ImVec2 p[4];
	for (int i = 0; i < 4; i++)
	{
		float d;
		if (!Project(corners[i], p[i].x, p[i].y, d)) { return; }
	}
	float alpha = Clamp01(a.elapsed / 0.4f) * (1.0f - a.disappear);
	if (alpha <= 0.0f) { return; }
	dl->AddImageQuad((ImTextureID)Cover_View(), p[0], p[1], p[2], p[3], ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), IM_COL32(255, 255, 255, (int)(alpha * 255.0f)));
}

static float SeedRand(unsigned& state)
{
	state = state * 1664525u + 1013904223u;
	return (float)((state >> 8) & 0xFFFFFF) / 16777216.0f;
}

static void DrawStar(ImDrawList* dl, ImVec2 c, float r, float angle, ImU32 col)
{
	ImVec2 inner[5];
	ImVec2 outer[5];
	for (int k = 0; k < 5; k++)
	{
		float ao = angle + k * 2.0f * Pi / 5.0f - Pi * 0.5f;
		float ai = ao + Pi / 5.0f;
		outer[k] = { c.x + std::cos(ao) * r, c.y + std::sin(ao) * r };
		inner[k] = { c.x + std::cos(ai) * r * 0.45f, c.y + std::sin(ai) * r * 0.45f };
	}
	dl->AddConvexPolyFilled(inner, 5, col);
	for (int k = 0; k < 5; k++)
	{
		dl->AddTriangleFilled(outer[k], inner[k], inner[(k + 4) % 5], col);
	}
}

static void DrawHeart(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
	dl->AddCircleFilled({ c.x - r * 0.5f, c.y - r * 0.2f }, r * 0.58f, col, 12);
	dl->AddCircleFilled({ c.x + r * 0.5f, c.y - r * 0.2f }, r * 0.58f, col, 12);
	dl->AddTriangleFilled({ c.x - r * 1.05f, c.y }, { c.x + r * 1.05f, c.y }, { c.x, c.y + r * 1.15f }, col);
}

static void DrawNote(ImDrawList* dl, ImVec2 c, float r, float tilt, ImU32 col)
{
	float ca = std::cos(tilt), sa = std::sin(tilt);
	auto rot = [&](float x, float y) { return ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca); };
	ImVec2 head[10];
	for (int k = 0; k < 10; k++)
	{
		float t = k * 2.0f * Pi / 10.0f;
		head[k] = rot(std::cos(t) * r * 0.75f, std::sin(t) * r * 0.55f);
	}
	dl->AddConvexPolyFilled(head, 10, col);
	float stem = std::max(1.0f, r * 0.22f);
	dl->AddLine(rot(r * 0.65f, 0.0f), rot(r * 0.65f, -r * 2.6f), col, stem);
	dl->AddTriangleFilled(rot(r * 0.65f, -r * 2.6f), rot(r * 1.6f, -r * 1.7f), rot(r * 0.65f, -r * 1.9f), col);
}

static void DrawPetal(ImDrawList* dl, ImVec2 c, float r, float angle, float squash, ImU32 col)
{
	float ca = std::cos(angle), sa = std::sin(angle);
	ImVec2 pts[12];
	for (int k = 0; k < 12; k++)
	{
		float t = k * 2.0f * Pi / 12.0f;
		float x = std::cos(t) * r;
		float y = std::sin(t) * r * 0.55f * squash * (1.0f - 0.35f * std::cos(t));
		pts[k] = { c.x + x * ca - y * sa, c.y + x * sa + y * ca };
	}
	dl->AddConvexPolyFilled(pts, 12, col);
}

static void DrawSparkle(ImDrawList* dl, ImVec2 c, float r, float angle, ImU32 col)
{
	for (int k = 0; k < 2; k++)
	{
		float a0 = angle + k * Pi * 0.5f;
		float ca = std::cos(a0), sa = std::sin(a0);
		ImVec2 q[4] = { { c.x + ca * r, c.y + sa * r }, { c.x - sa * r * 0.18f, c.y + ca * r * 0.18f }, { c.x - ca * r, c.y - sa * r }, { c.x + sa * r * 0.18f, c.y - ca * r * 0.18f } };
		dl->AddConvexPolyFilled(q, 4, col);
	}
}

static void DrawParticles(ImDrawList* dl, const SpawnedLine& line, const AnimState& a, int style, float amount)
{
	float life = 1.4f;
	if (style == Particles_Snow || style == Particles_Petals || style == Particles_Leaves) { life = 2.4f; }
	else if (style == Particles_Fireflies || style == Particles_Butterflies) { life = 2.8f; }
	else if (style == Particles_Smoke) { life = 2.0f; }
	else if (style == Particles_Rain) { life = 0.9f; }
	bool continuous = S.particlesContinuous;
	if (!continuous && a.elapsed > life + 0.6f) { return; }
	float halfH = line.height * 0.5f;
	float halfW = halfH * TextAspect(line.text);
	float distance = std::max(0.5f, Len(Sub(line.anchor, s_Cam.pos)));
	float pxPerMeter = s_Cam.height / (2.0f * distance * s_Cam.tanHalf);
	Color4 tint = S.karaoke ? s_LiveKaraoke : a.textColor;
	static const Color4 confetti[5] = { { 1.0f, 0.35f, 0.4f, 1.0f }, { 1.0f, 0.85f, 0.3f, 1.0f }, { 0.35f, 0.85f, 1.0f, 1.0f }, { 0.5f, 1.0f, 0.5f, 1.0f }, { 0.85f, 0.5f, 1.0f, 1.0f } };
	unsigned state = line.seed * 2654435761u + 17u;
	int count = (int)(28.0f * amount);
	for (int i = 0; i < count; i++)
	{
		float u = SeedRand(state) * 2.0f - 1.0f;
		float v = (SeedRand(state) * 2.0f - 1.0f) * 0.6f;
		float delay = (u + 1.0f) * 0.25f + SeedRand(state) * 0.15f;
		float r1 = SeedRand(state);
		float r2 = SeedRand(state);
		float r3 = SeedRand(state);
		float r4 = SeedRand(state);
		float t = a.elapsed - delay;
		if (continuous && t > 0.0f) { t = std::fmod(t + r4 * life, life); }
		if (t <= 0.0f || t >= life) { continue; }
		float k = t / life;
		V3 base = Add(Add(line.anchor, Mul(line.right, u * halfW)), Mul(line.up, v * halfH));
		V3 drift = { 0, 0, 0 };
		float sizeBase = 0.03f + r3 * 0.04f;
		switch (style)
		{
		case Particles_Notes:
			drift = Add(Mul(line.up, (0.6f + r1 * 0.5f) * t), Mul(line.right, std::sin(t * 3.0f + r2 * 6.0f) * 0.2f));
			sizeBase = 0.045f + r3 * 0.03f;
			break;
		case Particles_Petals:
			base = Add(base, Mul(line.up, halfH * 1.4f));
			drift = Add(Mul(line.up, -(0.3f + r1 * 0.3f) * t), Mul(line.right, (0.25f + r2 * 0.35f) * t + std::sin(t * 2.5f + r4 * 6.0f) * 0.12f));
			sizeBase = 0.05f + r3 * 0.04f;
			break;
		case Particles_Fireflies:
			drift = Add(Add(Mul(line.up, std::sin(t * (1.1f + r1) + r2 * 6.0f) * 0.25f + t * 0.1f), Mul(line.right, std::cos(t * (0.9f + r3) + r4 * 6.0f) * 0.35f)), Mul(line.back, -0.2f * r4));
			sizeBase = 0.02f + r3 * 0.02f;
			break;
		case Particles_Glitter:
			drift = Mul(line.up, 0.05f * t);
			sizeBase = 0.03f + r3 * 0.04f;
			break;
		case Particles_Leaves:
			base = Add(base, Mul(line.up, halfH * 1.4f));
			drift = Add(Mul(line.up, -(0.35f + r1 * 0.3f) * t), Mul(line.right, std::sin(t * 2.0f + r4 * 6.0f) * 0.35f + (r2 - 0.5f) * 0.4f * t));
			sizeBase = 0.06f + r3 * 0.04f;
			break;
		case Particles_Butterflies:
			drift = Add(Add(Mul(line.up, (0.25f + r1 * 0.3f) * t + std::sin(t * 4.0f + r2 * 6.0f) * 0.12f), Mul(line.right, (r2 - 0.5f) * 0.8f * t + std::cos(t * 1.5f + r4 * 6.0f) * 0.2f)), Mul(line.back, -0.15f * r4));
			sizeBase = 0.05f + r3 * 0.03f;
			break;
		case Particles_Smoke:
			drift = Add(Mul(line.up, (0.3f + r1 * 0.25f) * t), Mul(line.right, (r2 - 0.5f) * 0.3f * t));
			sizeBase = 0.06f + r3 * 0.05f;
			break;
		case Particles_Pixels:
			drift = Add(Mul(line.up, (0.6f + r1 * 0.5f) * t - 0.9f * t * t), Mul(line.right, (r2 - 0.5f) * 0.9f * t));
			sizeBase = 0.035f + r3 * 0.03f;
			break;
		case Particles_Rain:
			base = Add(base, Mul(line.up, halfH * 1.8f));
			drift = Add(Mul(line.up, -(2.2f + r1 * 0.8f) * t), Mul(line.right, -0.25f * t));
			sizeBase = 0.02f + r3 * 0.015f;
			break;
		case Particles_Rings:
			drift = { 0, 0, 0 };
			sizeBase = 0.05f + r3 * 0.05f;
			break;
		case Particles_Confetti:
			drift = Add(Mul(line.up, (0.9f + r1 * 0.6f) * t - 1.4f * t * t), Mul(line.right, (r2 - 0.5f) * 1.2f * t));
			sizeBase = 0.05f + r3 * 0.03f;
			break;
		case Particles_Embers:
			drift = Add(Mul(line.up, (0.8f + r1 * 0.9f) * t), Mul(line.right, std::sin(t * 6.0f + r2 * 6.0f) * 0.08f));
			sizeBase = 0.02f + r3 * 0.03f;
			break;
		case Particles_Bubbles:
			drift = Add(Mul(line.up, (0.4f + r1 * 0.4f) * t), Mul(line.right, std::sin(t * 4.0f + r2 * 6.0f) * 0.12f));
			sizeBase = 0.05f + r3 * 0.06f;
			break;
		case Particles_Snow:
			base = Add(base, Mul(line.up, halfH * 1.6f));
			drift = Add(Mul(line.up, -(0.35f + r1 * 0.25f) * t), Mul(line.right, std::sin(t * 2.0f + r2 * 6.0f) * 0.15f));
			sizeBase = 0.025f + r3 * 0.035f;
			break;
		case Particles_Hearts:
		case Particles_Stars:
			drift = Add(Mul(line.up, (0.5f + r1 * 0.5f) * t), Mul(line.right, (r2 - 0.5f) * 0.6f * t));
			sizeBase = 0.05f + r3 * 0.04f;
			break;
		default:
			drift = Add(Add(Mul(line.up, (0.5f + r1 * 0.6f) * t - 0.2f * t * t), Mul(line.right, (r2 - 0.5f) * 0.6f * t)), Mul(line.back, -r4 * 0.3f * t));
			break;
		}
		V3 p = Add(base, Mul(drift, line.height));
		float sx, sy, d;
		if (!Project(p, sx, sy, d)) { continue; }
		float fadeIn = std::min(1.0f, t * 6.0f);
		float alpha = (1.0f - k) * fadeIn * (1.0f - a.disappear);
		float radius = std::max(1.0f, sizeBase * line.height * pxPerMeter * (1.0f - k * 0.4f));
		ImVec2 c = { sx, sy };
		switch (style)
		{
		case Particles_Notes:
			DrawNote(dl, c, radius * 1.1f, std::sin(t * 3.0f + r2 * 6.0f) * 0.3f, ToU32(i % 3 == 0 ? confetti[i % 5] : tint, alpha));
			break;
		case Particles_Petals:
		{
			static const Color4 petals[3] = { { 1.0f, 0.72f, 0.8f, 1.0f }, { 1.0f, 0.86f, 0.9f, 1.0f }, { 0.96f, 0.6f, 0.72f, 1.0f } };
			DrawPetal(dl, c, radius * 1.3f, t * (1.5f + r4 * 2.0f) + r2 * 6.0f, 0.4f + 0.6f * std::fabs(std::cos(t * 3.0f + r1 * 5.0f)), ToU32(petals[i % 3], std::min(1.0f, alpha * 1.3f)));
			break;
		}
		case Particles_Fireflies:
		{
			float pulse = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(t * (3.0f + r1 * 3.0f) + r2 * 6.0f));
			float fireAlpha = std::min(1.0f, fadeIn * (1.0f - a.disappear)) * (k < 0.8f ? 1.0f : (1.0f - k) * 5.0f);
			Color4 glow = { 0.85f, 1.0f, 0.45f, 1.0f };
			dl->AddCircleFilled(c, radius * 4.0f, ToU32(glow, fireAlpha * pulse * 0.15f), 12);
			dl->AddCircleFilled(c, radius * 2.0f, ToU32(glow, fireAlpha * pulse * 0.35f), 10);
			dl->AddCircleFilled(c, radius, ToU32({ 1.0f, 1.0f, 0.8f, 1.0f }, fireAlpha * pulse), 8);
			break;
		}
		case Particles_Glitter:
		{
			float twinkle = std::max(0.0f, std::sin(t * (9.0f + r1 * 8.0f) + r2 * 6.0f));
			DrawSparkle(dl, c, radius * (1.2f + twinkle * 1.4f), r4 * Pi, ToU32(i % 2 ? confetti[i % 5] : Color4{ 1.0f, 1.0f, 1.0f, 1.0f }, alpha * (0.3f + 0.7f * twinkle)));
			break;
		}
		case Particles_Leaves:
		{
			static const Color4 leaves[4] = { { 0.95f, 0.55f, 0.15f, 1.0f }, { 0.85f, 0.3f, 0.12f, 1.0f }, { 0.95f, 0.8f, 0.25f, 1.0f }, { 0.55f, 0.7f, 0.2f, 1.0f } };
			float ang = t * (1.2f + r4 * 2.0f) + r2 * 6.0f;
			DrawPetal(dl, c, radius * 1.5f, ang, 0.5f + 0.5f * std::fabs(std::cos(t * 2.5f + r1 * 5.0f)), ToU32(leaves[i % 4], std::min(1.0f, alpha * 1.3f)));
			float ca = std::cos(ang), sa = std::sin(ang);
			dl->AddLine({ c.x - ca * radius * 1.4f, c.y - sa * radius * 1.4f }, { c.x + ca * radius * 1.4f, c.y + sa * radius * 1.4f }, ToU32({ 0.4f, 0.25f, 0.1f, 1.0f }, alpha * 0.8f), std::max(1.0f, radius * 0.12f));
			break;
		}
		case Particles_Butterflies:
		{
			float flap = std::fabs(std::sin(t * 14.0f + r2 * 6.0f));
			ImU32 wing = ToU32(i % 2 ? tint : confetti[i % 5], std::min(1.0f, alpha * 1.2f));
			float w = radius * 1.3f * (0.25f + 0.75f * flap);
			dl->AddTriangleFilled(c, { c.x - w, c.y - radius * 1.2f }, { c.x - w * 0.9f, c.y + radius * 0.3f }, wing);
			dl->AddTriangleFilled(c, { c.x + w, c.y - radius * 1.2f }, { c.x + w * 0.9f, c.y + radius * 0.3f }, wing);
			dl->AddTriangleFilled(c, { c.x - w * 0.7f, c.y + radius * 0.2f }, { c.x - w * 0.4f, c.y + radius * 0.9f }, wing);
			dl->AddTriangleFilled(c, { c.x + w * 0.7f, c.y + radius * 0.2f }, { c.x + w * 0.4f, c.y + radius * 0.9f }, wing);
			dl->AddLine({ c.x, c.y - radius * 0.6f }, { c.x, c.y + radius * 0.7f }, ToU32({ 0.15f, 0.1f, 0.1f, 1.0f }, alpha), std::max(1.0f, radius * 0.15f));
			break;
		}
		case Particles_Smoke:
		{
			float puff = radius * (1.0f + k * 2.5f);
			dl->AddCircleFilled(c, puff, ToU32({ 0.75f, 0.75f, 0.78f, 1.0f }, alpha * 0.22f), 16);
			dl->AddCircleFilled({ c.x + puff * 0.3f, c.y - puff * 0.2f }, puff * 0.7f, ToU32({ 0.85f, 0.85f, 0.88f, 1.0f }, alpha * 0.18f), 14);
			break;
		}
		case Particles_Pixels:
		{
			float side = radius * 1.2f;
			ImU32 px = ToU32(confetti[i % 5], std::min(1.0f, alpha * 1.3f));
			dl->AddRectFilled({ std::floor(c.x - side), std::floor(c.y - side) }, { std::floor(c.x + side), std::floor(c.y + side) }, px);
			break;
		}
		case Particles_Rain:
		{
			float len = radius * 6.0f;
			dl->AddLine({ c.x + len * 0.12f, c.y - len }, c, ToU32({ 0.7f, 0.82f, 1.0f, 1.0f }, alpha * 0.8f), std::max(1.0f, radius * 0.5f));
			break;
		}
		case Particles_Rings:
		{
			float grow = 0.3f + k * 3.5f;
			dl->AddCircle(c, radius * grow * 2.0f, ToU32(tint, alpha * 0.8f), 24, std::max(1.0f, radius * 0.35f * (1.0f - k)));
			break;
		}
		case Particles_Confetti:
		{
			float ang = t * (4.0f + r4 * 6.0f) + r2 * 6.0f;
			float w = radius;
			float h = radius * 0.5f * (0.3f + 0.7f * std::fabs(std::cos(t * 7.0f + r1 * 5.0f)));
			float ca = std::cos(ang), sa = std::sin(ang);
			ImVec2 q[4] = { { c.x + (-w * ca - -h * sa), c.y + (-w * sa + -h * ca) }, { c.x + (w * ca - -h * sa), c.y + (w * sa + -h * ca) },
			                { c.x + (w * ca - h * sa), c.y + (w * sa + h * ca) }, { c.x + (-w * ca - h * sa), c.y + (-w * sa + h * ca) } };
			dl->AddQuadFilled(q[0], q[1], q[2], q[3], ToU32(confetti[i % 5], std::min(1.0f, alpha * 1.4f)));
			break;
		}
		case Particles_Embers:
		{
			float flicker = 0.6f + 0.4f * std::sin(t * 25.0f + r2 * 10.0f);
			Color4 hot = { 1.0f, 0.55f + 0.35f * (1.0f - k), 0.15f, 1.0f };
			dl->AddCircleFilled(c, radius * 2.5f, ToU32(hot, alpha * 0.2f * flicker), 10);
			dl->AddCircleFilled(c, radius, ToU32(hot, alpha * flicker), 8);
			break;
		}
		case Particles_Bubbles:
			dl->AddCircle(c, radius, ToU32({ 0.8f, 0.95f, 1.0f, 1.0f }, alpha * 0.8f), 16, std::max(1.0f, radius * 0.15f));
			dl->AddCircleFilled({ c.x - radius * 0.35f, c.y - radius * 0.35f }, radius * 0.22f, ToU32({ 1.0f, 1.0f, 1.0f, 1.0f }, alpha * 0.7f), 8);
			break;
		case Particles_Snow:
			dl->AddCircleFilled(c, radius * 1.8f, ToU32({ 1.0f, 1.0f, 1.0f, 1.0f }, alpha * 0.2f), 10);
			dl->AddCircleFilled(c, radius, ToU32({ 1.0f, 1.0f, 1.0f, 1.0f }, alpha * 0.9f), 8);
			break;
		case Particles_Stars:
		{
			float twinkle = 0.75f + 0.25f * std::sin(t * 12.0f + r2 * 6.0f);
			DrawStar(dl, c, radius * 1.4f * twinkle, t * 2.0f + r1 * 6.0f, ToU32(tint, alpha));
			break;
		}
		case Particles_Hearts:
			DrawHeart(dl, c, radius, ToU32({ 1.0f, 0.38f, 0.58f, 1.0f }, alpha));
			break;
		default:
			dl->AddCircleFilled(c, radius * 2.2f, ToU32(tint, alpha * 0.25f), 10);
			dl->AddCircleFilled(c, radius, ToU32({ 1.0f, 1.0f, 1.0f, 1.0f }, alpha * 0.9f), 8);
			break;
		}
	}
}

static bool DrawWorldText(ImDrawList* dl, ImFont* font, const SpawnedLine& line, const AnimState& a)
{
	const char* text = line.text.c_str();
	V3 right = line.right;
	if (S.readableBack && Dot(s_Cam.r, right) < 0.0f) { right = Mul(right, -1.0f); }

	float localSize = font->FontSize;
	ImVec2 ts = font->CalcTextSizeA(localSize, FLT_MAX, 0.0f, text);
	if (ts.x > s_Cam.width - 8.0f)
	{
		localSize *= (s_Cam.width - 8.0f) / ts.x;
		ts = font->CalcTextSizeA(localSize, FLT_MAX, 0.0f, text);
	}
	if (ts.y <= 0.0f) { return false; }

	TextFrame f;
	f.font = font;
	f.localSize = localSize;
	f.halfW = ts.x * 0.5f;
	f.halfH = ts.y * 0.5f;
	f.metersPerUnit = line.height / ts.y;
	f.height = line.height;
	f.anchor = line.anchor;
	f.right = right;
	f.back = line.back;
	f.up = line.up;

	float pixelHeight = 0.0f;
	{
		float ax, ay, ad, bx, by, bd;
		V3 top = Add(line.anchor, Mul(line.up, line.height * 0.5f));
		V3 bottom = Add(line.anchor, Mul(line.up, -line.height * 0.5f));
		if (Project(top, ax, ay, ad) && Project(bottom, bx, by, bd))
		{
			pixelHeight = std::fabs(by - ay);
		}
		else
		{
			float distance = std::max(0.5f, Len(Sub(line.anchor, s_Cam.pos)));
			pixelHeight = line.height / (2.0f * distance * s_Cam.tanHalf) * s_Cam.height;
		}
	}

	float depth = line.height * S.extrudeDepth;
	bool extrude = S.extrude;

	if (S.shadow)
	{
		float shadowPx = pixelHeight * 0.06f;
		if (shadowPx < 1.5f) { shadowPx = 1.5f; }
		if (shadowPx > 8.0f) { shadowPx = 8.0f; }
		V3 backOffset = extrude ? Mul(line.back, depth) : V3{ 0, 0, 0 };
		EmitLayer(dl, f, text, ToU32(S.shadowColor), backOffset, shadowPx, shadowPx, a);
	}

	bool viewingFront = Dot(Sub(line.anchor, s_Cam.pos), line.back) > 0.0f;
	if (!viewingFront)
	{
		EmitFrontFace(dl, f, text, pixelHeight, a);
	}

	if (extrude)
	{
		int layers = S.extrudeLayers;
		if (S.autoLayers)
		{
			float maxGap = 0.0f;
			const float sampleX[3] = { 0.0f, f.halfW, f.halfW * 2.0f };
			const float sampleY[2] = { 0.0f, f.halfH * 2.0f };
			for (int sx = 0; sx < 3; sx++)
			{
				for (int sy = 0; sy < 2; sy++)
				{
					V3 front = LocalToWorld(f, sampleX[sx], sampleY[sy]);
					V3 back = Add(front, Mul(line.back, depth));
					float ax, ay, ad, bx, by, bd;
					if (Project(front, ax, ay, ad) && Project(back, bx, by, bd))
					{
						float gap = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
						if (gap > maxGap) { maxGap = gap; }
					}
				}
			}
			layers = (int)std::ceil(maxGap / 0.75f);
			if (layers < 2) { layers = 2; }
			if (layers > S.maxLayers) { layers = S.maxLayers; }
		}
		for (int n = 0; n < layers; n++)
		{
			int i = viewingFront ? (layers - n) : (n + 1);
			float t = (float)i / (float)layers;
			float shade = 1.0f - 0.47f * t;
			Color4 c = { a.depthColor[0] * shade, a.depthColor[1] * shade, a.depthColor[2] * shade, a.depthColor[3] };
			EmitLayer(dl, f, text, ToU32(c), Mul(line.back, depth * t), 0.0f, 0.0f, a);
		}
	}

	if (viewingFront)
	{
		EmitFrontFace(dl, f, text, pixelHeight, a);
	}
	return true;
}

static ImFont* CurrentFont()
{
	for (const auto& entry : s_Fonts)
	{
		if (entry.name == S.fontName && entry.font) { return entry.font; }
	}
	return s_Nexus->FontBig ? (ImFont*)s_Nexus->FontBig : ImGui::GetFont();
}

static ImFont* FontFor(const std::string& text)
{
	ImFont* main = CurrentFont();
	if (!s_Script.font || !main) { return main; }
	const char* s = text.c_str();
	const char* end = s + text.size();
	while (s < end)
	{
		unsigned int c = (unsigned int)(unsigned char)*s;
		if (c < 0x80) { s++; continue; }
		int n = ImTextCharFromUtf8(&c, s, end);
		if (n <= 0 || c == 0) { break; }
		s += n;
		if (c < 0x10000 && !main->FindGlyphNoFallback((ImWchar)c) && s_Script.font->FindGlyphNoFallback((ImWchar)c)) { return s_Script.font; }
	}
	return main;
}

static void CollectScriptChars(const std::string& text, std::set<unsigned>& out)
{
	const char* s = text.c_str();
	const char* end = s + text.size();
	while (s < end)
	{
		unsigned int c = (unsigned int)(unsigned char)*s;
		if (c < 0x80) { s++; continue; }
		int n = ImTextCharFromUtf8(&c, s, end);
		if (n <= 0 || c == 0) { break; }
		s += n;
		if (c < 0x100 || c >= 0x10000 || (c >= 0x2010 && c <= 0x2027)) { continue; }
		out.insert(c);
	}
}

static bool HasCharIn(const std::set<unsigned>& chars, unsigned lo, unsigned hi)
{
	auto it = chars.lower_bound(lo);
	return it != chars.end() && *it <= hi;
}

static std::string ScriptFontPath(const std::set<unsigned>& chars)
{
	char windir[MAX_PATH];
	UINT len = GetWindowsDirectoryA(windir, MAX_PATH);
	if (len == 0 || len >= MAX_PATH - 40) { return ""; }
	std::string fonts = std::string(windir) + "\\Fonts\\";
	std::vector<const char*> files;
	if (HasCharIn(chars, 0x1100, 0x11FF) || HasCharIn(chars, 0x3130, 0x318F) || HasCharIn(chars, 0xAC00, 0xD7AF)) { files = { "malgunbd.ttf", "malgun.ttf" }; }
	else if (HasCharIn(chars, 0x3040, 0x30FF)) { files = { "YuGothB.ttc", "meiryob.ttc", "meiryo.ttc", "msgothic.ttc" }; }
	else if (HasCharIn(chars, 0x4E00, 0x9FFF)) { files = { "msyhbd.ttc", "msyh.ttc", "simsun.ttc" }; }
	else { files = { "segoeuib.ttf", "arialbd.ttf", "arial.ttf" }; }
	for (const char* f : files)
	{
		std::string path = fonts + f;
		if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) { return path; }
	}
	return "";
}

static void FreeScriptBuild(ScriptBuild* build)
{
	if (!build) { return; }
	if (build->atlas) { IM_DELETE(build->atlas); }
	delete build;
}

static void ReleaseScriptFont()
{
	if (s_ScriptThread.joinable()) { s_ScriptThread.join(); }
	s_ScriptBusy = false;
	{
		std::lock_guard<std::mutex> lock(s_ScriptLock);
		FreeScriptBuild(s_ScriptDone);
		s_ScriptDone = nullptr;
	}
	if (s_Script.view) { s_Script.view->Release(); }
	if (s_Script.atlas) { IM_DELETE(s_Script.atlas); }
	s_Script = ScriptFont();
	s_ScriptWanted.clear();
}

static void BuildScriptAtlas(ScriptBuild* build, std::string path)
{
	build->atlas = IM_NEW(ImFontAtlas)();
	ImFontConfig cfg;
	cfg.OversampleH = 1;
	cfg.OversampleV = 1;
	cfg.PixelSnapH = true;
	build->atlas->TexDesiredWidth = 4096;
	if (build->atlas->AddFontFromFileTTF(path.c_str(), 96.0f, &cfg, build->ranges.data()) && build->atlas->Build())
	{
		build->atlas->GetTexDataAsRGBA32(&build->pixels, &build->width, &build->height);
		build->ok = build->pixels && build->width > 0 && build->height > 0 && build->height <= 16384;
	}
	std::lock_guard<std::mutex> lock(s_ScriptLock);
	FreeScriptBuild(s_ScriptDone);
	s_ScriptDone = build;
}

static void FinishScriptFont()
{
	ScriptBuild* build = nullptr;
	{
		std::lock_guard<std::mutex> lock(s_ScriptLock);
		build = s_ScriptDone;
		s_ScriptDone = nullptr;
	}
	if (!build) { return; }
	if (s_ScriptThread.joinable()) { s_ScriptThread.join(); }
	s_ScriptBusy = false;
	ID3D11ShaderResourceView* view = nullptr;
	ID3D11Device* device = nullptr;
	if (build->ok && s_API->SwapChain && SUCCEEDED(((IDXGISwapChain*)s_API->SwapChain)->GetDevice(__uuidof(ID3D11Device), (void**)&device)) && device)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = (UINT)build->width;
		desc.Height = (UINT)build->height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA init{};
		init.pSysMem = build->pixels;
		init.SysMemPitch = (UINT)build->width * 4;
		ID3D11Texture2D* texture = nullptr;
		if (SUCCEEDED(device->CreateTexture2D(&desc, &init, &texture)) && texture)
		{
			device->CreateShaderResourceView(texture, nullptr, &view);
			texture->Release();
		}
		device->Release();
	}
	if (!view)
	{
		FreeScriptBuild(build);
		return;
	}
	build->atlas->ClearTexData();
	build->atlas->TexID = (ImTextureID)view;
	if (s_Script.view) { s_Script.view->Release(); }
	if (s_Script.atlas) { IM_DELETE(s_Script.atlas); }
	s_Script.atlas = build->atlas;
	s_Script.font = build->atlas->Fonts.Size > 0 ? build->atlas->Fonts[0] : nullptr;
	s_Script.view = view;
	s_Script.chars = build->chars;
	build->atlas = nullptr;
	FreeScriptBuild(build);
}

static std::string ScriptDebug()
{
	char buf[200];
	unsigned probe = 0;
	for (auto it = s_Lines.rbegin(); it != s_Lines.rend() && !probe; ++it)
	{
		const char* s = it->text.c_str();
		const char* end = s + it->text.size();
		while (s < end)
		{
			unsigned int c = (unsigned int)(unsigned char)*s;
			if (c < 0x80) { s++; continue; }
			int n = ImTextCharFromUtf8(&c, s, end);
			if (n <= 0 || c == 0) { break; }
			s += n;
			if (c >= 0x100) { probe = c; break; }
		}
	}
	ImFont* main = CurrentFont();
	int mainHas = probe && main && main->FindGlyphNoFallback((ImWchar)probe) ? 1 : 0;
	int scriptHas = probe && s_Script.font && s_Script.font->FindGlyphNoFallback((ImWchar)probe) ? 1 : 0;
	snprintf(buf, sizeof(buf), " | extra letters %s wanted %d glyphs %d probe U+%04X main %d script %d",
		s_Script.font ? "ready" : (s_ScriptBusy ? "loading" : "none"), (int)s_ScriptWanted.size(),
		s_Script.font ? s_Script.font->Glyphs.Size : -1, probe, mainHas, scriptHas);
	return buf;
}

static void UpdateScriptFont(const LyricsSnapshot& lyrics, const MediaInfo& media)
{
	FinishScriptFont();
	if (s_ScriptBusy) { return; }
	std::string title = media.artist + " " + media.title;
	const void* source = lyrics.status == LyricsStatus::Found && lyrics.lines ? (const void*)lyrics.lines.get() : nullptr;
	if (source == s_ScriptSource && title == s_ScriptTitle) { return; }
	s_ScriptSource = source;
	s_ScriptTitle = title;
	std::set<unsigned> chars;
	CollectScriptChars(title, chars);
	if (source) { for (const auto& l : *lyrics.lines) { CollectScriptChars(l.text, chars); } }
	if (chars.empty()) { return; }
	while (chars.size() > 2500) { chars.erase(std::prev(chars.end())); }
	if (std::includes(s_ScriptWanted.begin(), s_ScriptWanted.end(), chars.begin(), chars.end())) { return; }
	std::string path = ScriptFontPath(chars);
	if (path.empty()) { return; }

	s_ScriptWanted = chars;
	ScriptBuild* build = new ScriptBuild();
	build->chars = chars;
	build->ranges = { 0x0020, 0x00FF, 0x2010, 0x2027 };
	unsigned lo = 0, hi = 0;
	bool open = false;
	for (unsigned c : chars)
	{
		if (open && c == hi + 1) { hi = c; continue; }
		if (open) { build->ranges.push_back((ImWchar)lo); build->ranges.push_back((ImWchar)hi); }
		lo = hi = c;
		open = true;
	}
	if (open) { build->ranges.push_back((ImWchar)lo); build->ranges.push_back((ImWchar)hi); }
	build->ranges.push_back(0);
	if (s_ScriptThread.joinable()) { s_ScriptThread.join(); }
	s_ScriptBusy = true;
	s_ScriptThread = std::thread(BuildScriptAtlas, build, path);
}

static const char* StatusName(LyricsStatus s)
{
	switch (s)
	{
	case LyricsStatus::Loading:  return "loading";
	case LyricsStatus::Found:    return "found";
	case LyricsStatus::NotFound: return "not found";
	case LyricsStatus::Error:    return "error";
	default:                     return "idle";
	}
}

static std::vector<std::string> SplitWords(const std::string& text)
{
	std::vector<std::string> words;
	std::string current;
	for (char c : text)
	{
		if (c == ' ')
		{
			if (!current.empty()) { words.push_back(current); current.clear(); }
		}
		else
		{
			current += c;
		}
	}
	if (!current.empty()) { words.push_back(current); }
	return words;
}

static std::string PlainPunctuation(const std::string& text)
{
	struct Swap { const char* from; const char* to; };
	static const Swap swaps[] = {
		{ "\xE2\x80\x98", "'" }, { "\xE2\x80\x99", "'" }, { "\xE2\x80\x9A", "'" }, { "\xE2\x80\xB2", "'" },
		{ "\xE2\x80\x9C", "\"" }, { "\xE2\x80\x9D", "\"" }, { "\xE2\x80\x9E", "\"" },
		{ "\xE2\x80\x93", "-" }, { "\xE2\x80\x94", "-" }, { "\xE2\x80\xA6", "..." }, { "\xC2\xA0", " " }
	};
	std::string out = text;
	for (const auto& sw : swaps)
	{
		size_t len = strlen(sw.from);
		size_t pos = 0;
		while ((pos = out.find(sw.from, pos)) != std::string::npos)
		{
			out.replace(pos, len, sw.to);
			pos += strlen(sw.to);
		}
	}
	return out;
}

static std::string LowerLetters(const std::string& word);

static std::string BalanceBrackets(const std::string& text)
{
	int round = 0, square = 0, missingRound = 0, missingSquare = 0;
	for (char c : text)
	{
		if (c == '(') { round++; }
		else if (c == ')') { if (round > 0) { round--; } else { missingRound++; } }
		else if (c == '[') { square++; }
		else if (c == ']') { if (square > 0) { square--; } else { missingSquare++; } }
	}
	if (!round && !square && !missingRound && !missingSquare) { return text; }
	return std::string(missingSquare, '[') + std::string(missingRound, '(') + text + std::string(round, ')') + std::string(square, ']');
}

static std::vector<bool> FindChorus(const std::vector<LyricLine>& src)
{
	std::vector<std::string> keys(src.size());
	std::map<std::string, int> counts;
	for (size_t i = 0; i < src.size(); i++)
	{
		keys[i] = LowerLetters(PlainPunctuation(src[i].text));
		if (keys[i].size() >= 6) { counts[keys[i]]++; }
	}
	auto repeats = [&](size_t i) { auto it = counts.find(keys[i]); return it == counts.end() ? 0 : it->second; };
	std::vector<bool> chorus(src.size(), false);
	for (size_t i = 0; i < src.size(); i++)
	{
		int n = repeats(i);
		if (n >= 3) { chorus[i] = true; continue; }
		if (n == 2)
		{
			bool neighbor = (i > 0 && repeats(i - 1) >= 2) || (i + 1 < src.size() && repeats(i + 1) >= 2);
			chorus[i] = neighbor;
		}
	}
	return chorus;
}

static void RebuildExpanded(const std::vector<LyricLine>& src, double songDuration)
{
	s_Expanded.clear();
	std::vector<bool> chorus = FindChorus(src);
	for (size_t i = 0; i < src.size(); i++)
	{
		double start = src[i].time;
		double next = i + 1 < src.size() ? src[i + 1].time : start + 6.0;
		if (i + 1 >= src.size() && songDuration > start) { next = std::min(next, songDuration); }
		const std::string text = PlainPunctuation(src[i].text);

		int length = (int)text.size();
		double span = std::min(next - start, std::max(1.5, length * 0.08));
		int chunks = 1;
		if (S.splitLong && S.maxChars > 4 && length > S.maxChars)
		{
			chunks = (length + S.maxChars - 1) / S.maxChars;
			int byTime = (int)(span / 0.9);
			if (byTime < 1) { byTime = 1; }
			if (chunks > byTime) { chunks = byTime; }
		}

		std::vector<std::string> words;
		{
			std::vector<std::string> raw = SplitWords(text);
			size_t i = 0;
			while (i < raw.size())
			{
				std::string key = LowerLetters(raw[i]);
				size_t j = i + 1;
				while (j < raw.size() && !key.empty() && LowerLetters(raw[j]) == key) { j++; }
				if (j - i >= 3)
				{
					std::string run;
					for (size_t k = i; k < j; k++) { if (!run.empty()) { run += ' '; } run += raw[k]; }
					words.push_back(run);
					i = j;
				}
				else
				{
					words.push_back(raw[i]);
					i++;
				}
			}
		}
		if (chunks <= 1 || words.size() < 2)
		{
			s_Expanded.push_back({ start, text, chorus[i] });
			continue;
		}
		if ((int)words.size() < chunks) { chunks = (int)words.size(); }

		std::vector<std::string> pieces(chunks);
		std::vector<int> piecePos(chunks, -1);
		int pos = 0;
		int total = 0;
		for (const auto& w : words) { total += (int)w.size() + 1; }
		for (const auto& w : words)
		{
			int mid = pos + (int)w.size() / 2;
			int index = (int)((long long)mid * chunks / std::max(1, total));
			if (index >= chunks) { index = chunks - 1; }
			if (piecePos[index] < 0) { piecePos[index] = pos; }
			if (!pieces[index].empty()) { pieces[index] += ' '; }
			pieces[index] += w;
			pos += (int)w.size() + 1;
		}

		for (int k = 0; k < chunks; k++)
		{
			if (pieces[k].empty()) { continue; }
			double frac = (double)std::max(0, piecePos[k]) / (double)std::max(1, total);
			s_Expanded.push_back({ start + span * frac, BalanceBrackets(pieces[k]), chorus[i] });
		}
	}
}

static void SplitBacking(std::string& main, std::string& backing)
{
	std::string outside;
	std::string inside;
	int depth = 0;
	for (char c : main)
	{
		if (c == '(') { depth++; if (depth == 1 && !inside.empty()) { inside += ' '; } continue; }
		if (c == ')' && depth > 0) { depth--; continue; }
		if (depth > 0) { inside += c; } else { outside += c; }
	}
	auto trim = [](std::string t)
	{
		std::string out;
		bool space = false;
		for (char c : t)
		{
			if (c == ' ' || c == '\t') { space = !out.empty(); continue; }
			if (space) { out += ' '; space = false; }
			out += c;
		}
		return out;
	};
	outside = trim(outside);
	inside = trim(inside);
	if (inside.empty()) { return; }
	main = outside;
	backing = inside;
}

static double SingTime(const std::string& text)
{
	double syllables = 0.0;
	bool vowelRun = false;
	bool wordHasVowel = false;
	bool inWord = false;
	const char* s = text.c_str();
	const char* end = s + text.size();
	auto endWord = [&]() { if (inWord && !wordHasVowel) { syllables += 1.0; } inWord = false; wordHasVowel = false; vowelRun = false; };
	while (s < end)
	{
		unsigned int c = (unsigned int)(unsigned char)*s;
		if (c >= 0x80)
		{
			int n = ImTextCharFromUtf8(&c, s, end);
			if (n <= 0 || c == 0) { break; }
			s += n;
			endWord();
			if (c >= 0x2E80) { syllables += 1.0; }
			else { syllables += 0.35; }
			continue;
		}
		s++;
		char l = (char)std::tolower((int)c);
		bool letter = (l >= 'a' && l <= 'z') || l == '\'';
		if (!letter) { endWord(); continue; }
		inWord = true;
		bool vowel = l == 'a' || l == 'e' || l == 'i' || l == 'o' || l == 'u' || l == 'y';
		if (vowel && !vowelRun) { syllables += 1.0; wordHasVowel = true; }
		vowelRun = vowel;
	}
	endWord();
	return 0.25 + std::max(1.0, syllables) * 0.24;
}

static void SpawnLyricLine(const std::string& text, double now, double start, double next, double position, float scale, int voice, bool chorus)
{
	SpawnLine(text, now);
	SpawnedLine& spawned = s_Lines.back();
	spawned.voice = voice;
	spawned.chorus = chorus && S.chorusBoost;
	if (spawned.chorus) { scale *= 1.25f; }
	spawned.height *= scale;
	double gap = std::max(0.3, std::min(15.0, next - start));
	double sing = std::max(0.4, std::min(gap * 0.95, SingTime(text)));
	spawned.singDuration = (float)std::max(0.3, start + sing - position);
	spawned.lyricStart = start;
	spawned.lyricEnd = start + sing;
	int glyphs = 0;
	int total = 0;
	for (unsigned char ch : text) { if ((ch & 0xC0) != 0x80) { total++; } }
	int charIndex = 0;
	float wordStart = 0.0f;
	bool inWord = false;
	for (unsigned char ch : text)
	{
		if ((ch & 0xC0) == 0x80) { continue; }
		bool space = ch == ' ' || ch == '\t';
		if (!space && !inWord) { wordStart = (float)charIndex / (float)std::max(1, total); }
		inWord = !space;
		if (!space)
		{
			spawned.wordFrac.push_back(wordStart);
			glyphs++;
		}
		charIndex++;
	}
	spawned.glyphCount = glyphs;
	if (scale != 1.0f)
	{
		BuildLayout(spawned, FontFor(spawned.text));
		spawned.settled = spawned.pinned;
		spawned.settleDeadline = now + 0.2;
	}
}

static std::string LowerLetters(const std::string& word)
{
	std::string out;
	for (unsigned char c : word)
	{
		if (c >= 'A' && c <= 'Z') { out += (char)(c - 'A' + 'a'); }
		else if ((c >= 'a' && c <= 'z') || c >= 0x80) { out += (char)c; }
	}
	return out;
}

static bool IsBadWord(const std::string& w)
{
	static const char* prefixes[] = { "fuck", "motherfuck", "shit", "bullshit", "bitch", "cunt", "nigg", "pussy", "whore", "slut", "dickhead", "asshole", "faggot", "goddamn" };
	static const char* exact[] = { "ass", "asses", "dick", "dicks", "cock", "cocks", "damn", "hoe", "hoes", "fag", "fags", "twat", "retard", "retarded", "bastard", "bastards", "dumbass", "jackass", "wanker", "piss", "pissed", "tits", "titties" };
	if (w.size() < 3) { return false; }
	for (const char* p : prefixes) { if (w.compare(0, strlen(p), p) == 0) { return true; } }
	for (const char* e : exact) { if (w == e) { return true; } }
	return false;
}

static std::string CensorText(const std::string& text)
{
	std::string out = text;
	size_t i = 0;
	while (i < out.size())
	{
		while (i < out.size() && !std::isalpha((unsigned char)out[i])) { i++; }
		size_t start = i;
		while (i < out.size() && (std::isalpha((unsigned char)out[i]) || out[i] == '\'')) { i++; }
		if (i <= start) { continue; }
		std::string word = out.substr(start, i - start);
		if (!IsBadWord(LowerLetters(word))) { continue; }
		size_t first = start;
		size_t last = i - 1;
		while (last > first && !std::isalpha((unsigned char)out[last])) { last--; }
		for (size_t k = first + 1; k < last; k++) { if (std::isalpha((unsigned char)out[k])) { out[k] = '*'; } }
	}
	return out;
}

static void BuildPlan(const std::string& text, double start, double next)
{
	s_Plan.clear();
	s_PlanNext = 0;
	double span = std::max(0.3, std::min(15.0, next - start));
	std::string main = S.censor ? CensorText(text) : text;
	std::string backing;
	if (S.splitBacking) { SplitBacking(main, backing); }
	double singSpan = std::max(0.4, std::min(span * 0.95, SingTime(main)));

	struct Word { std::string text; size_t offset; bool glue; };
	std::vector<Word> words;
	{
		auto isJoin = [](char c) { return c == '-' || c == ',' || c == '/' || c == '~'; };
		size_t i = 0;
		bool glue = false;
		while (i < main.size())
		{
			if (main[i] == ' ' || main[i] == '\t') { i++; glue = false; continue; }
			size_t begin = i;
			while (i < main.size() && main[i] != ' ' && main[i] != '\t')
			{
				char c = main[i++];
				if (isJoin(c) && i < main.size() && main[i] != ' ' && main[i] != '\t') { break; }
			}
			words.push_back({ main.substr(begin, i - begin), begin, glue });
			glue = i < main.size() && main[i] != ' ' && main[i] != '\t';
		}
	}
	auto cleanAlone = [](std::string w)
	{
		while (!w.empty() && (w.back() == ',' || w.back() == '-' || w.back() == '/' || w.back() == '~' || w.back() == '.')) { w.pop_back(); }
		return w.empty() ? std::string("-") : w;
	};
	double total = (double)std::max<size_t>(1, main.size());
	auto timeOf = [&](size_t offset) { return start + singSpan * ((double)offset / total); };

	std::vector<bool> alone(words.size(), S.wordMode == 2);
	if (S.splitRepeats && S.wordMode != 2)
	{
		size_t i = 0;
		while (i < words.size())
		{
			std::string key = LowerLetters(words[i].text);
			size_t j = i + 1;
			while (j < words.size() && !key.empty() && LowerLetters(words[j].text) == key) { j++; }
			if (j - i >= 3) { for (size_t k = i; k < j; k++) { alone[k] = true; } }
			i = j;
		}
	}

	std::string rest;
	bool lastKept = false;
	for (size_t i = 0; i < words.size(); i++)
	{
		if (alone[i]) { lastKept = false; continue; }
		if (!rest.empty() && !(words[i].glue && lastKept)) { rest += ' '; }
		rest += words[i].text;
		lastKept = true;
	}
	if (!rest.empty()) { s_Plan.push_back({ start, next, BalanceBrackets(rest), 1.0f, 0 }); }
	for (size_t i = 0; i < words.size(); i++)
	{
		if (!alone[i]) { continue; }
		double t = std::max(start, timeOf(words[i].offset));
		double end = i + 1 < words.size() ? std::max(t + 0.15, timeOf(words[i + 1].offset)) : std::max(t + 0.3, start + singSpan);
		s_Plan.push_back({ t, end, BalanceBrackets(cleanAlone(words[i].text)), S.wordMode == 2 ? 1.0f : 0.9f, 0 });
	}
	if (!backing.empty()) { s_Plan.push_back({ start, next, BalanceBrackets(S.censor ? CensorText(backing) : backing), 0.8f, 1 }); }
	std::stable_sort(s_Plan.begin(), s_Plan.end(), [](const PlannedSpawn& a, const PlannedSpawn& b) { return a.time < b.time; });
}

static void FadeOldGroups(double now, int keepGroups)
{
	for (auto& line : s_Lines)
	{
		if (line.fadeStart > 0.0) { continue; }
		if (line.isTitle) { continue; }
		if (line.group < 0 || line.group <= s_Group - keepGroups) { line.fadeStart = now; }
	}
}

static float SongOffset(const std::string& key)
{
	auto it = S.songOffsets.find(key);
	return it != S.songOffsets.end() ? it->second : 0.0f;
}

static void UpdateLyrics(double now, const MediaInfo& media, const LyricsSnapshot& lyrics, bool hidden)
{
	std::string key = media.available ? MakeTrackKey(media.artist, media.title) : std::string();
	if (key != s_LastTrackKey)
	{
		s_LastTrackKey = key;
		s_LastIndex = -2;
		FadeAllLines(now);
		bool fresh = s_EverHadTrack || media.position < 8.0;
		if (!key.empty()) { s_EverHadTrack = true; }
		if (S.titleCard && S.showLyrics && !hidden && media.available && !media.title.empty() && fresh)
		{
			bool withCover = S.coverArt && media.cover && media.coverSize > 0;
			std::string name = PlainPunctuation(media.artist.empty() ? media.title : media.title + "  -  " + media.artist);
			if (S.censor) { name = CensorText(name); }
			SpawnLine(withCover ? std::string(7, ' ') + name : name, now);
			s_Lines.back().isTitle = true;
			s_Lines.back().coverVersion = withCover ? media.coverVersion : -1;
			s_TitleShown = true;
		}
	}

	if (!S.showLyrics || !media.available || media.title.empty()) { return; }
	if (lyrics.status != LyricsStatus::Found || !lyrics.lines || lyrics.trackKey != key) { return; }

	if (s_ExpandedSource != lyrics.lines.get() || s_ExpandedSplit != S.splitLong || s_ExpandedMaxChars != S.maxChars)
	{
		RebuildExpanded(*lyrics.lines, media.duration);
		s_ExpandedSource = lyrics.lines.get();
		s_ExpandedSplit = S.splitLong;
		s_ExpandedMaxChars = S.maxChars;
		s_LastIndex = -2;
	}

	double position = media.position + S.lyricsOffset + SongOffset(key);
	s_LivePosition = position;
	auto it = std::upper_bound(s_Expanded.begin(), s_Expanded.end(), position, [](double p, const TimedLine& l) { return p < l.time; });
	int index = (int)(it - s_Expanded.begin()) - 1;

	if (s_TitleShown && index < 0) { return; }
	s_TitleShown = false;
	if (index != s_LastIndex)
	{
		s_LastIndex = index;
		s_Group++;
		FadeOldGroups(now, std::max(1, S.keepLines));
		s_Plan.clear();
		s_PlanNext = 0;
		if (index >= 0 && !s_Expanded[index].text.empty())
		{
			double start = s_Expanded[index].time;
			double next = index + 1 < (int)s_Expanded.size() ? s_Expanded[index + 1].time : start + 4.0;
			BuildPlan(s_Expanded[index].text, start, next);
			s_PlanChorus = s_Expanded[index].chorus;
		}
	}
	while (s_PlanNext < s_Plan.size() && position >= s_Plan[s_PlanNext].time - 0.02)
	{
		const PlannedSpawn& item = s_Plan[s_PlanNext++];
		if (hidden || position > item.end + 0.5) { continue; }
		SpawnLyricLine(item.text, now, item.time, item.end, position, item.scale, item.voice, s_PlanChorus);
		s_Lines.back().group = s_Group;
	}
}

static void SetShowLyrics(bool show);

static const char* FormatName(unsigned f)
{
	switch (f)
	{
	case DXGI_FORMAT_D24_UNORM_S8_UINT:    return "D24S8";
	case DXGI_FORMAT_R24G8_TYPELESS:       return "R24G8 typeless";
	case DXGI_FORMAT_D32_FLOAT:            return "D32F";
	case DXGI_FORMAT_R32_TYPELESS:         return "R32 typeless";
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32F S8";
	case DXGI_FORMAT_R32G8X24_TYPELESS:    return "R32G8X24 typeless";
	case DXGI_FORMAT_D16_UNORM:            return "D16";
	case DXGI_FORMAT_R16_TYPELESS:         return "R16 typeless";
	default:                               return "other";
	}
}

static float  s_LiveNear = 0.0f;
static double s_LiveNearTime = -100.0;
static float  s_LiveNearRaw = 0.0f;
static float  s_LiveSpread = 0.0f;
static int    s_LiveCount = 0;
static float  s_BodyImplied[3] = { 0, 0, 0 };

static float SceneDepthMeters(float raw)
{
	if (raw <= 0.0f || raw >= 0.99999f) { return 1e9f; }
	return s_NearPlane / std::max(1e-6f, 1.0f - raw + s_DepthK);
}

struct DepthPair
{
	float z;
	float y;
};
static std::vector<V3>        s_Trail;
static size_t                 s_TrailCursor = 0;
static std::vector<DepthPair> s_Pairs;
static float  s_FitC = 0.0f;
static float  s_FitK = 0.0f;
static float  s_FitRms = 0.0f;
static float  s_FitRange = 0.0f;
static int    s_FitUsed = 0;
static bool   s_FitValid = false;
static double s_FitTime = -100.0;

static bool FitDepth(float& c, float& k, float& rms, float& range, int& used)
{
	if (s_Pairs.size() < 80) { return false; }
	std::vector<char> keep(s_Pairs.size(), 1);
	for (int pass = 0; pass < 4; pass++)
	{
		double n = 0, sz = 0, sy = 0, szz = 0, szy = 0;
		for (size_t i = 0; i < s_Pairs.size(); i++)
		{
			if (!keep[i]) { continue; }
			double z = s_Pairs[i].z, y = s_Pairs[i].y;
			n += 1; sz += z; sy += y; szz += z * z; szy += z * y;
		}
		if (n < 60) { return false; }
		double den = n * szz - sz * sz;
		if (den < 1e-6) { return false; }
		double slope = (n * szy - sz * sy) / den;
		double icept = (sy - slope * sz) / n;
		std::vector<float> res;
		for (size_t i = 0; i < s_Pairs.size(); i++)
		{
			if (!keep[i]) { continue; }
			res.push_back((float)std::fabs(s_Pairs[i].y - (icept + slope * s_Pairs[i].z)));
		}
		std::vector<float> sorted = res;
		std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
		float mad = std::max(0.004f, sorted[sorted.size() / 2]);
		c = (float)icept;
		k = (float)-slope;
		double sse = 0;
		int count = 0;
		std::vector<float> zs;
		for (size_t i = 0; i < s_Pairs.size(); i++)
		{
			if (!keep[i]) { continue; }
			float r = (float)std::fabs(s_Pairs[i].y - (icept + slope * s_Pairs[i].z));
			if (r > mad * 3.0f) { keep[i] = 0; continue; }
			sse += r * r;
			count++;
			zs.push_back(s_Pairs[i].z);
		}
		rms = count > 0 ? (float)std::sqrt(sse / count) : 1.0f;
		used = count;
		if (zs.size() >= 10)
		{
			std::sort(zs.begin(), zs.end());
			range = zs[zs.size() * 9 / 10] - zs[zs.size() / 10];
		}
	}
	return used >= 60 && range >= 8.0f && k > -0.002f && k < 0.05f && c > 0.15f && c < 2.0f && rms < c * 0.03f;
}

static std::vector<float> s_GroundScale;

static float Percentile(std::vector<float> v, float q)
{
	if (v.empty()) { return 0.0f; }
	size_t k = (size_t)(q * (v.size() - 1) + 0.5f);
	std::nth_element(v.begin(), v.begin() + k, v.end());
	return v[k];
}

static void UpdateDepthChecks()
{
	std::vector<DepthSample> request;
	request.push_back({ s_Cam.width * 0.5f, s_Cam.height * 0.5f, 0.0f, -1.0f, false, -1, 0 });
	V3 feet = ToV3(s_Mumble->AvatarPosition);
	const float bodyHeights[3] = { 0.5f, 1.0f, 1.5f };
	for (int i = 0; i < 3; i++)
	{
		V3 p = Add(feet, { 0.0f, bodyHeights[i], 0.0f });
		float x, y, z;
		if (Project(p, x, y, z)) { request.push_back({ x, y, z, -1.0f, false, -2, i }); }
	}
	bool mounted = s_Mumble->Context.MountIndex != Mumble::EMountIndex::None;
	for (int i = 0; i < 12; i++)
	{
		float radius = i < 4 ? 0.6f : (mounted ? 3.2f : 1.5f);
		float angle = i < 4 ? i * Pi * 0.5f : (i - 4) * Pi * 0.25f + Pi * 0.125f;
		V3 p = Add(feet, { std::cos(angle) * radius, 0.0f, std::sin(angle) * radius });
		float x, y, z;
		if (Project(p, x, y, z)) { request.push_back({ x, y, z, -1.0f, false, -5, i }); }
	}
	{
		bool walking = s_Mumble->Context.MountIndex == Mumble::EMountIndex::None && std::fabs(s_MoveVel.y) < 0.6f;
		if (walking && (s_Trail.empty() || Len(Sub(s_Trail.back(), feet)) > 1.5f))
		{
			s_Trail.push_back(feet);
			if (s_Trail.size() > 80) { s_Trail.erase(s_Trail.begin()); }
		}
		int added = 0;
		for (size_t n = 0; n < s_Trail.size() && added < 20; n++)
		{
			size_t i = (s_TrailCursor + n) % s_Trail.size();
			float x, y, z;
			if (Project(s_Trail[i], x, y, z) && z > 3.0f && z < 90.0f && x >= 0.0f && y >= 0.0f && x < s_Cam.width && y < s_Cam.height)
			{
				request.push_back({ x, y, z, -1.0f, false, -7, (int)i });
				added++;
			}
		}
		if (!s_Trail.empty()) { s_TrailCursor = (s_TrailCursor + 7) % s_Trail.size(); }
	}
	Depth_RequestSamples(request);

	auto results = Depth_GetSamples();
	for (const auto& r : results)
	{
		bool use = r.tag == -7 || ((r.tag == -2 || (r.tag == -5 && r.index < 4)) && s_Mumble->Context.MountIndex == Mumble::EMountIndex::None && std::fabs(s_MoveVel.y) < 0.6f);
		if (!use || !r.valid || r.raw <= 0.0f || r.raw >= 0.99999f) { continue; }
		if (r.expectedZ < 2.0f || r.expectedZ > 90.0f) { continue; }
		float y = r.expectedZ * (1.0f - r.raw);
		if (y < 0.05f || y > 3.0f) { continue; }
		s_Pairs.push_back({ r.expectedZ, y });
	}
	if (s_Pairs.size() > 1500) { s_Pairs.erase(s_Pairs.begin(), s_Pairs.begin() + (s_Pairs.size() - 1500)); }
	if ((s_FrameId % 20) == 0)
	{
		float c = 0, k = 0, rms = 0, range = 0;
		int used = 0;
		s_FitValid = FitDepth(c, k, rms, range, used);
		s_FitC = c; s_FitK = k; s_FitRms = rms; s_FitRange = range; s_FitUsed = used;
		if (s_FitValid)
		{
			s_FitTime = ImGui::GetTime();
			std::string key = std::to_string(s_LastMapId);
			auto ic = S.nearByMap.find(key);
			auto ik = S.depthKByMap.find(key);
			if (ic == S.nearByMap.end() || ik == S.depthKByMap.end() || std::fabs(ic->second - c) > c * 0.01f || std::fabs(ik->second - k) > 0.0005f)
			{
				S.nearByMap[key] = c;
				S.depthKByMap[key] = k;
				MarkDirty(true);
			}
		}
	}
	bool checkedAny = false;
	bool matchedAny = false;
	for (const auto& r : results)
	{
		if (!r.valid || r.raw < 0.0f || r.tag != -2) { continue; }
		if (r.expectedZ < 3.5f || r.expectedZ > 40.0f) { continue; }
		if (r.screenX < 0.0f || r.screenY < 0.0f || r.screenX >= s_Cam.width || r.screenY >= s_Cam.height) { continue; }
		checkedAny = true;
		float scene = SceneDepthMeters(r.raw);
		if (std::fabs(scene - r.expectedZ) < 0.6f + r.expectedZ * 0.05f) { matchedAny = true; }
		if (r.index >= 0 && r.index < 3) { s_BodyImplied[r.index] = r.expectedZ * (1.0f - r.raw); }
	}
	bool onFoot = s_Mumble->Context.MountIndex == Mumble::EMountIndex::None && std::fabs(s_MoveVel.y) < 0.6f;
	std::vector<float> frameImplied;
	std::vector<float> bodyImplied;
	for (const auto& r : results)
	{
		if (!onFoot || !r.valid || (r.tag != -5 && r.tag != -2)) { continue; }
		if (r.raw <= 0.0f || r.raw >= 0.99999f) { continue; }
		if (r.expectedZ < 2.0f || r.expectedZ > 60.0f) { continue; }
		if (r.screenX < 0.0f || r.screenY < 0.0f || r.screenX >= s_Cam.width || r.screenY >= s_Cam.height) { continue; }
		float implied = r.expectedZ * (1.0f - r.raw);
		if (implied < 0.05f || implied > 3.0f) { continue; }
		if (r.tag == -5) { s_GroundScale.push_back(implied); }
		else { bodyImplied.push_back(implied); }
		frameImplied.push_back(implied);
	}
	if (frameImplied.size() >= 3)
	{
		std::sort(frameImplied.begin(), frameImplied.end());
		float med = frameImplied[frameImplied.size() / 2];
		std::vector<float> close;
		for (float v : frameImplied) { if (std::fabs(v - med) < med * 0.03f) { close.push_back(v); } }
		s_LiveNearRaw = med;
		s_LiveSpread = (frameImplied.back() - frameImplied.front()) / std::max(0.001f, med);
		bool bodyAgrees = false;
		if (bodyImplied.size() >= 2)
		{
			std::sort(bodyImplied.begin(), bodyImplied.end());
			float bodyMid = bodyImplied[bodyImplied.size() / 2];
			int agree = 0;
			for (float v : bodyImplied) { if (std::fabs(v - bodyMid) < bodyMid * 0.03f) { agree++; } }
			if (agree >= 2 && std::fabs(bodyMid - med) > med * 0.03f)
			{
				med = bodyMid;
				close.clear();
				for (float v : frameImplied) { if (std::fabs(v - med) < med * 0.03f) { close.push_back(v); } }
			}
			bodyAgrees = agree >= 2;
		}
		if (close.size() >= 3 && (bodyAgrees || close.size() * 2 >= frameImplied.size()) && med >= 0.15f && med <= 1.5f)
		{
			float sum = 0.0f;
			for (float v : close) { sum += v; }
			float value = sum / (float)close.size();
			bool fresh = ImGui::GetTime() - s_LiveNearTime < 1.0;
			s_LiveNear = fresh ? s_LiveNear + (value - s_LiveNear) * 0.2f : value;
			s_LiveNearTime = ImGui::GetTime();
			s_LiveCount++;
		}
	}
	if (s_GroundScale.size() > 900) { s_GroundScale.erase(s_GroundScale.begin(), s_GroundScale.begin() + (s_GroundScale.size() - 900)); }

	if (checkedAny && !results.empty())
	{
		s_CheckBatches++;
		if (matchedAny) { s_CheckMatches++; }
		if (s_CheckBatches >= 300)
		{
			s_CheckBatches = 0;
			s_CheckMatches = 0;
		}
	}

	if (!s_DepthWindow) { return; }

	ImDrawList* bg = ImGui::GetBackgroundDrawList();
	if (s_GridReady)
	{
		for (int cy = 1; cy < s_GridH; cy += 3)
		{
			for (int cx = 1; cx < s_GridW; cx += 3)
			{
				int i = cy * s_GridW + cx;
				float n = s_GridNear[i];
				float f = s_GridFar[i];
				V3 world = GridPoint(cx, cy, n > 1e8f ? 1000.0f : n);
				float gx, gy, gz;
				if (!Project(world, gx, gy, gz)) { continue; }
				ImU32 col;
				if (n > 1e8f) { col = IM_COL32(80, 160, 255, 150); }
				else if (f > 1e8f || !s_GridFlat[i]) { col = IM_COL32(255, 0, 255, 170); }
				else if (n < 10.0f) { col = IM_COL32(255, 70, 70, 150); }
				else if (n < 30.0f) { col = IM_COL32(255, 220, 60, 150); }
				else { col = IM_COL32(80, 255, 120, 150); }
				bg->AddCircleFilled({ gx, gy }, 2.5f, col, 6);
			}
		}
	}
	for (const auto& r : request)
	{
		if (r.tag == -1) { bg->AddCircle({ r.screenX, r.screenY }, 6.0f, IM_COL32(0, 255, 255, 255), 12, 2.0f); }
		if (r.tag == -2) { bg->AddCircle({ r.screenX, r.screenY }, 5.0f, IM_COL32(255, 0, 255, 255), 12, 2.0f); }
	}
	static const char* kindNames[] = { "air", "wall", "close" };
	static const ImU32 kindColors[] = { IM_COL32(90, 255, 120, 230), IM_COL32(255, 170, 40, 230), IM_COL32(255, 80, 80, 230) };
	for (const auto& line : s_Lines)
	{
		float halfH = line.height * 0.5f;
		float halfW = halfH * TextAspect(line.text);
		float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
		bool any = false;
		for (int c = 0; c < 4; c++)
		{
			V3 q = Add(Add(line.anchor, Mul(line.right, ((c & 1) ? 1.0f : -1.0f) * halfW)), Mul(line.up, ((c & 2) ? 1.0f : -1.0f) * halfH));
			float sx, sy, z;
			if (Project(q, sx, sy, z)) { any = true; x1 = std::min(x1, sx); x2 = std::max(x2, sx); y1 = std::min(y1, sy); y2 = std::max(y2, sy); }
		}
		if (!any) { continue; }
		float hidden = -1.0f;
		BlockedFraction(line, hidden);
		int fixedLetters = 0;
		for (const auto& t : line.liftTarget) { if (Len(t) > 0.01f) { fixedLetters++; } }
		int kind = std::max(0, std::min(2, line.kind));
		char label[160];
		snprintf(label, sizeof(label), "#%u %s %.1f m | clear %d%% | hidden %s%d%% | fixed %d | moved %d%s", line.id, kindNames[kind], Len(Sub(line.anchor, s_Cam.pos)), line.spawnClear,
			hidden < 0.0f ? "?" : "", hidden < 0.0f ? 0 : (int)(hidden * 100.0f + 0.5f), fixedLetters, line.relocations, line.fadeStart > 0.0 ? " | fading" : "");
		bg->AddRect({ x1, y1 }, { x2, y2 }, kindColors[kind], 0.0f, 0, 2.0f);
		bg->AddText({ x1, y2 + 4.0f }, kindColors[kind], label);
	}
	if (s_MinimapRect.valid) { bg->AddRect({ s_MinimapRect.x1, s_MinimapRect.y1 }, { s_MinimapRect.x2, s_MinimapRect.y2 }, IM_COL32(255, 255, 0, 200), 0.0f, 0, 2.0f); }
	if (s_TrackerRect.valid) { bg->AddRect({ s_TrackerRect.x1, s_TrackerRect.y1 }, { s_TrackerRect.x2, s_TrackerRect.y2 }, IM_COL32(255, 128, 0, 200), 0.0f, 0, 2.0f); }
	if (s_CharacterRect.valid) { bg->AddRect({ s_CharacterRect.x1, s_CharacterRect.y1 }, { s_CharacterRect.x2, s_CharacterRect.y2 }, IM_COL32(0, 255, 255, 200), 0.0f, 0, 2.0f); }

	ImGui::SetNextWindowSize(ImVec2(580, 440), ImGuiCond_FirstUseEver);
	if (ImGui::Begin(ADDON_NAME " depth test", &s_DepthWindow, ImGuiWindowFlags_NoCollapse))
	{
		DepthStats st = Depth_GetStats();
		ImGui::Text("Status: %s", Depth_GetStatus().c_str());
		ImGui::TextColored(ImVec4(0.4f, 1.0f, 1.0f, 1.0f), "DEPTH c %.4f k %.5f | fit %s c %.4f k %.5f rms %.4f range %.1f m used %d of %d | trail %d | body %.3f %.3f %.3f | cam %.1f m", s_NearPlane, s_DepthK,
			s_FitValid ? "OK" : "no", s_FitC, s_FitK, s_FitRms, s_FitRange, s_FitUsed, (int)s_Pairs.size(), (int)s_Trail.size(), s_BodyImplied[0], s_BodyImplied[1], s_BodyImplied[2], Len(Sub(s_Cam.pos, ToV3(s_Mumble->AvatarPosition))));
		ImGui::Text("Copies per second: %d | frames since copy: %d | buffer switches: %d", st.copiesLastSecond, st.framesSinceCopy, st.switches);
		{
			std::vector<DepthPass> passes = Depth_GetPasses();
			int firstBack = -1;
			int totalDraws = 0;
			for (int i = 0; i < (int)passes.size(); i++)
			{
				totalDraws += passes[i].draws;
				if (firstBack < 0 && passes[i].backBuffer) { firstBack = i; }
			}
			ImGui::Text("Draws last frame: game thread %d | other threads %d | command lists %d", st.immediateDraws, st.otherDraws, st.commandLists);
			ImGui::TextWrapped("Hooks: %s", Depth_HookReport().c_str());
			ImGui::TextWrapped("Behind the UI: %s", Depth_UiStatus().c_str());
			if (ImGui::Button("Save UI test images") && !s_AddonDir.empty()) { Depth_RequestDump(s_AddonDir); }
			{
				void* before = nullptr;
				void* after = nullptr;
				Depth_GetUiMask(before, after);
				if (before && after)
				{
					ImGui::Image((ImTextureID)before, ImVec2(192, 108));
					ImGui::SameLine();
					ImGui::Image((ImTextureID)after, ImVec2(192, 108));
				}
			}
			ImGui::Text("Last frame: %d target binds, %d draws, first back buffer bind #%d", (int)passes.size(), totalDraws, firstBack);
			{
				std::string compact;
				for (int i = 0; i < (int)passes.size(); i++)
				{
					const DepthPass& p = passes[i];
					char item[96];
					if (p.targets < 0) { snprintf(item, sizeof(item), "#%d %s>SCREEN(%u fmt%u)", i, p.targets == -1 ? "COPY" : "RESOLVE", p.width, p.format); }
					else { snprintf(item, sizeof(item), "#%d %u fmt%u%s%s d%d s%d", i, p.width, p.format, p.backBuffer ? " SCREEN" : "", p.depth ? " z" : "", p.draws, p.smallDraws); }
					if (!compact.empty()) { compact += "  |  "; }
					compact += item;
				}
				ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x);
				ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", compact.c_str());
				ImGui::PopTextWrapPos();
			}
		}
		ImGui::Text("Near plane: %.4f m | measured on foot: %.4f m", s_NearPlane, s_LiveNear);
		ImGui::Text("Per-pixel hiding: %s", Occlusion_Status().c_str());
		ImGui::Text("Sensor grid: %s | %d x %d | %d frames old", s_GridReady ? "ready" : Depth_GridStatus().c_str(), s_GridW, s_GridH, s_GridAge > 999 ? -1 : s_GridAge);
		ImGui::Text("Last line clear: %d%% | on wall: %s | moved lines: %d", s_LastSpawnClear, s_LastSpawnWall ? "yes" : "no", s_Relocated);
		ImGui::Text("Letters checked: %d | letters fixed: %d", s_ProbeCount, s_LiftedCount);
		ImGui::Text("Depth check: character found in %d of the last %d checks", s_CheckMatches, s_CheckBatches);
		if (s_GroundScale.size() >= 30)
		{
			float mid = Percentile(s_GroundScale, 0.5f);
			float lo = Percentile(s_GroundScale, 0.25f);
			float hi = Percentile(s_GroundScale, 0.75f);
			float off = (s_NearPlane / std::max(0.001f, mid) - 1.0f) * 100.0f;
			ImGui::TextColored(std::fabs(off) > 3.0f ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f) : ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
				"Ground scale: measured %.4f (middle 50%%: %.4f to %.4f, %d samples) | in use %.4f | off by %+.1f%%", mid, lo, hi, (int)s_GroundScale.size(), s_NearPlane, off);
		}
		else
		{
			ImGui::Text("Ground scale check: collecting (%d samples, stand on the ground with your feet on screen)", (int)s_GroundScale.size());
		}
		ImGui::TextDisabled("Dots: red under 10 m, yellow under 30 m, green farther, blue sky, pink edges");
		ImGui::TextDisabled("Boxes: green air, orange wall, red close fallback");
		ImGui::Separator();
		for (const auto& c : Depth_GetCandidates())
		{
			ImGui::Text("%s %u x %u  %s  draws %d  binds %d", c.chosen ? ">>" : "  ", c.width, c.height, FormatName(c.format), c.draws, c.views);
		}
		ImGui::Separator();
		for (const auto& r : results)
		{
			if (r.tag == -1 && r.valid) { ImGui::Text("Center: %.2f m away", SceneDepthMeters(r.raw)); }
			if (r.tag == -2 && r.valid) { ImGui::Text("Body %d: scene %.2f m | expected %.2f m", r.index, SceneDepthMeters(r.raw), r.expectedZ); }
			if (r.tag == -5 && r.valid) { ImGui::Text("Ground %d: scene %.2f m | expected %.2f m", r.index, SceneDepthMeters(r.raw), r.expectedZ); }
		}
	}
	ImGui::End();
}

static Color4 OppositeColor(const Color4& c)
{
	float hi = std::max(c[0], std::max(c[1], c[2]));
	float lo = std::min(c[0], std::min(c[1], c[2]));
	if (hi - lo < 0.12f) { return { 0.55f, 0.85f, 1.0f, c[3] }; }
	Color4 o = { hi + lo - c[0], hi + lo - c[1], hi + lo - c[2], c[3] };
	float top = std::max(o[0], std::max(o[1], o[2]));
	if (top > 0.01f && top < 0.85f) { for (int k = 0; k < 3; k++) { o[k] = std::min(1.0f, o[k] * 0.85f / top); } }
	return o;
}

static V3 FromCamera(V3 v)
{
	return Add(Add(Mul(s_Cam.r, v.x), Mul(s_Cam.u, v.y)), Mul(s_Cam.f, v.z));
}

static void AddonRender()
{
	if (!s_Nexus || !s_Mumble) { return; }
	if (s_Nexus->Width == 0 || s_Nexus->Height == 0) { return; }

	double now = ImGui::GetTime();
	EnsureFontLoaded();
	if (s_Dirty && now - s_DirtyTime > 1.0) { SaveSettings(); }
	if (!s_Dirty && now - s_SettingsCheck > 1.0)
	{
		s_SettingsCheck = now;
		FILETIME ft;
		if (SettingsFileTime(ft) && CompareFileTime(&ft, &s_SettingsTime) != 0)
		{
			LoadSettings();
			s_SettingsTime = ft;
		}
	}
	if (s_ToggleRequested.exchange(false)) { SetShowLyrics(!S.showLyrics); }
	if (s_DepthWindowRequested.exchange(false)) { s_DepthWindow = !s_DepthWindow; }
	if (s_TestKeyRequested.exchange(false)) { s_TestRequested = true; }
	if (int nudge = s_NudgeRequested.exchange(0))
	{
		if (!s_LastTrackKey.empty())
		{
			float value = SongOffset(s_LastTrackKey) + 0.2f * (float)nudge;
			value = std::max(-10.0f, std::min(10.0f, std::round(value * 100.0f) / 100.0f));
			if (std::fabs(value) < 0.005f) { S.songOffsets.erase(s_LastTrackKey); }
			else { S.songOffsets[s_LastTrackKey] = value; }
			MarkDirty(true);
			char text[96];
			snprintf(text, sizeof(text), ADDON_NAME ": this song %+.1f s (%s)", value, value >= 0.0f ? "earlier" : "later");
			s_API->GUI_SendAlert(text);
		}
	}

	s_FrameId++;
	UpdateCamera();
	bool wantDepth = S.occlusion || S.smartPlacement || S.liftLetters || s_DepthWindow;
	if (wantDepth && !Depth_IsInstalled() && !s_DepthFailed && s_API->SwapChain)
	{
		if (!Depth_Install(s_API->SwapChain)) { s_DepthFailed = true; }
	}
	else if (!wantDepth && Depth_IsInstalled())
	{
		Occlusion_Shutdown();
		Depth_Uninstall();
		ResetGrid();
	}
	bool gameplay = s_Nexus->IsGameplay;
	if (s_Mumble->Context.MapID != s_LastMapId || (gameplay && !s_WasGameplay))
	{
		s_LastMapId = s_Mumble->Context.MapID;
		s_GroundScale.clear();
		s_LiveCount = 0;
		s_Trail.clear();
		s_Pairs.clear();
		s_FitValid = false;
		if (Depth_IsInstalled()) { Depth_ResetTracking(); }
		ResetGrid();
		s_CheckBatches = 0;
		s_CheckMatches = 0;
	}
	s_WasGameplay = gameplay;
	if (S.nearPlane < 0.2f || S.nearPlane > 0.8f) { S.nearPlane = 0.288f; }
	s_DepthK = 0.0f;
	{
		std::string key = std::to_string(s_LastMapId);
		auto known = S.nearByMap.find(key);
		auto knownK = S.depthKByMap.find(key);
		if (s_FitValid && now - s_FitTime < 5.0) { s_MeasuredNear = s_FitC; s_DepthK = s_FitK; }
		else if (known != S.nearByMap.end()) { s_MeasuredNear = known->second; s_DepthK = knownK != S.depthKByMap.end() ? knownK->second : 0.0f; }
		else if (s_LiveNear > 0.0f) { s_MeasuredNear = s_LiveNear; }
		else { s_MeasuredNear = S.nearPlane; }
	}
	s_NearPlane = S.autoNearPlane ? s_MeasuredNear : S.nearPlane;
	if (!S.autoNearPlane) { s_DepthK = 0.0f; }

	if (Depth_IsInstalled()) { Depth_SetUiMaskEnabled(S.behindUi); Depth_SetDiagnostics(s_DepthWindow); Depth_EndFrame(s_Cam.width, s_Cam.height, s_FrameId); }
	PullGrid();
	UpdateScreenRects();
	MediaInfo media = Media_Get();
	LyricsSnapshot lyrics = Lyrics_Get();
	UpdateScriptFont(lyrics, media);
	if (S.beatPulse)
	{
		Audio_SetSource(media.appId);
		Audio_Start();
	}
	else { Audio_Stop(); }
	float beat = S.beatPulse ? Audio_Pulse() * S.beatAmount : 0.0f;
	{
		V3 avatarNow = ToV3(s_Mumble->AvatarPosition);
		float dt = ImGui::GetIO().DeltaTime;
		if (s_HavePrevAvatar && dt > 0.0f && dt < 0.5f)
		{
			V3 raw = Mul(Sub(avatarNow, s_PrevAvatar), 1.0f / dt);
			if (Len(raw) < 40.0f)
			{
				float k = std::min(1.0f, dt * 4.0f);
				s_MoveVel = Add(s_MoveVel, Mul(Sub(raw, s_MoveVel), k));
			}
			else { s_MoveVel = { 0, 0, 0 }; }
		}
		s_PrevAvatar = avatarNow;
		s_HavePrevAvatar = true;
	}
	if (S.titleCard && S.coverArt && media.cover && media.coverSize > 0 && media.coverVersion != Cover_Version())
	{
		Cover_Update(s_API->SwapChain, media.coverVersion, media.cover->data(), media.coverSize, media.coverSize);
	}
	{
		Color4 wantText = S.textColor;
		Color4 wantDepth = S.depthColor;
		Color4 wantKaraoke = S.karaokeColor;
		if (S.coverColors && media.available && media.hasAccent)
		{
			for (int c = 0; c < 3; c++)
			{
				wantText[c] = media.accent[c];
				wantDepth[c] = media.accent[c] * 0.45f;
				if (S.karaokeFromCover) { wantKaraoke[c] = media.accent2[c]; }
			}
		}
		float blend = std::min(1.0f, ImGui::GetIO().DeltaTime * 3.0f);
		for (int c = 0; c < 4; c++)
		{
			s_LiveText[c] += (wantText[c] - s_LiveText[c]) * blend;
			s_LiveDepth[c] += (wantDepth[c] - s_LiveDepth[c]) * blend;
			s_LiveKaraoke[c] += (wantKaraoke[c] - s_LiveKaraoke[c]) * blend;
		}
	}

	bool canDraw = s_Nexus->IsGameplay && !s_Mumble->Context.IsMapOpen;
	const auto& ctx = s_Mumble->Context;
	bool hidden = (S.hideInCombat && ctx.IsInCombat) || (S.hideInPvP && ctx.IsCompetitive) || (S.hideWhileTyping && ctx.IsTextboxFocused);
	if (hidden != s_WasHidden)
	{
		if (hidden) { FadeAllLines(now); }
		else { s_LastIndex = -2; }
		s_WasHidden = hidden;
	}
	if (canDraw)
	{
		UpdateLyrics(now, media, lyrics, hidden);
		if (s_TestRequested)
		{
			s_TestRequested = false;
			FadeAllLines(now);
			SpawnLine("Feels like I'm going insane", now);
		}
	}

	for (auto& line : s_Lines)
	{
		if (line.fadeStart <= 0.0 && now - line.start > (line.isTitle ? std::max(6.0f, S.maxOnScreen) : S.maxOnScreen))
		{
			line.fadeStart = now;
		}
	}
	s_Lines.erase(std::remove_if(s_Lines.begin(), s_Lines.end(), [&](const SpawnedLine& l)
	{
		return l.fadeStart > 0.0 && now - l.fadeStart > S.disappearTime;
	}), s_Lines.end());

	if (!canDraw) { return; }

	float dt = std::min(0.1f, ImGui::GetIO().DeltaTime);
	if (S.relocate && GridUsable())
	{
		std::vector<SpawnedLine> respawn;
		for (auto& line : s_Lines)
		{
			if (line.fadeStart > 0.0 || now - line.start < 0.8 || line.pinned) { continue; }
			float fraction = 0.0f;
			if (BlockedFraction(line, fraction))
			{
				if (fraction > 0.6f) { line.blockedTime += dt; }
				else { line.blockedTime = std::max(0.0f, line.blockedTime - dt * 2.0f); }
			}
			if (line.blockedTime > 1.0f && line.relocations < 2)
			{
				line.fadeStart = now;
				respawn.push_back(line);
			}
		}
		for (const auto& old : respawn)
		{
			SpawnLine(old.text, now);
			SpawnedLine& moved = s_Lines.back();
			moved.relocations = old.relocations + 1;
			moved.voice = old.voice;
			moved.chorus = old.chorus;
			moved.group = old.group;
			moved.isTitle = old.isTitle;
			moved.lyricStart = old.lyricStart;
			moved.lyricEnd = old.lyricEnd;
			moved.singDuration = old.singDuration;
			moved.glyphCount = old.glyphCount;
			moved.wordFrac = old.wordFrac;
			if (old.chorus) { moved.height *= 1.25f; BuildLayout(moved, FontFor(moved.text)); }
			s_Relocated++;
		}
	}

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	Occlusion_SetDepthK(s_DepthK);
	Occlusion_BeginFrame(s_Cam.tanHalf, s_Cam.aspect, s_Cam.width, s_Cam.height, s_NearPlane, 0.15f,
		s_MinimapRect.valid ? s_MinimapRect.x1 : 0.0f, s_MinimapRect.valid ? s_MinimapRect.y1 : 0.0f,
		s_MinimapRect.valid ? s_MinimapRect.x2 : 0.0f, s_MinimapRect.valid ? s_MinimapRect.y2 : 0.0f);
	bool occlude = S.occlusion && Occlusion_Ready();
	int checked = 0;
	int fixed = 0;
	for (auto& line : s_Lines)
	{
		if (line.pinned)
		{
			line.anchor = Add(s_Cam.pos, FromCamera(line.camAnchor));
			line.right = FromCamera(line.camRight);
			line.up = FromCamera(line.camUp);
			line.back = FromCamera(line.camBack);
		}
		else if (S.faceMe && !line.onWall)
		{
			Placement turn;
			turn.anchor = line.anchor;
			FaceCamera(turn);
			line.right = turn.right;
			line.up = turn.up;
			line.back = turn.back;
		}
		ImFont* font = FontFor(line.text);
		if (line.layoutFont != (const void*)font)
		{
			BuildLayout(line, font);
			line.settled = false;
			line.settleDeadline = now + 0.2;
		}
		if (!S.liftLetters)
		{
			std::fill(line.lift.begin(), line.lift.end(), V3{ 0, 0, 0 });
			std::fill(line.liftTarget.begin(), line.liftTarget.end(), V3{ 0, 0, 0 });
		}
		else if (!line.settled && line.fadeStart <= 0.0)
		{
			if (now <= line.settleDeadline) { SettleLetters(line); }
			else { line.settled = true; }
		}
		checked += (int)line.glyphs.size();
		for (const auto& t : line.liftTarget) { if (Len(t) > 0.01f) { fixed++; } }
		float follow = std::min(1.0f, dt * 10.0f);
		V3 front = Mul(line.back, -1.0f);
		float maxOff = 0.0f;
		for (size_t i = 0; i < line.lift.size() && i < line.liftTarget.size(); i++)
		{
			line.lift[i] = Add(line.lift[i], Mul(Sub(line.liftTarget[i], line.lift[i]), follow));
			maxOff = std::max(maxOff, Dot(line.lift[i], front));
		}

		AnimState a;
		a.elapsed = (float)(now - line.start);
		a.disappear = line.fadeStart > 0.0 && S.disappearTime > 0.0f ? Clamp01((float)(now - line.fadeStart) / S.disappearTime) : 0.0f;
		a.seed = line.seed;
		a.lineId = line.id;
		a.lift = S.liftLetters ? &line.lift : nullptr;
		a.beat = beat * (1.0f - a.disappear) * (line.chorus ? 1.8f : 1.0f);
		bool other = line.voice != 0 && S.otherVoiceColor;
		a.textColor = other ? OppositeColor(s_LiveText) : s_LiveText;
		if (other)
		{
			Color4 od = OppositeColor(s_LiveText);
			a.depthColor = { od[0] * 0.45f, od[1] * 0.45f, od[2] * 0.45f, s_LiveDepth[3] };
		}
		else { a.depthColor = s_LiveDepth; }
		a.chorusGlow = line.chorus;
		a.glowColor = S.glow ? S.glowColor : Color4{ s_LiveKaraoke[0], s_LiveKaraoke[1], s_LiveKaraoke[2], 0.6f };
		a.glyphCount = line.glyphCount > 0 ? line.glyphCount : (int)line.glyphs.size();
		float progress = -1.0f;
		if (line.lyricEnd > line.lyricStart)
		{
			progress = Clamp01((float)((s_LivePosition - line.lyricStart) / (line.lyricEnd - line.lyricStart)));
		}
		else if (line.singDuration > 0.0f)
		{
			progress = Clamp01(a.elapsed / line.singDuration);
		}
		if (S.karaoke && progress >= 0.0f && line.glyphCount > 0)
		{
			a.litGlyphs = progress * (float)line.glyphCount;
			a.litColor = ToU32(other ? s_LiveText : s_LiveKaraoke);
		}
		if (S.wordMode == 1 && progress >= 0.0f && line.fadeStart <= 0.0 && !line.wordFrac.empty())
		{
			a.progress = std::min(1.0f, progress * 1.15f);
			a.wordFrac = &line.wordFrac;
		}
		a.front = front;
		bool split = occlude && maxOff > 0.002f;
		a.pass = split ? 1 : 0;

		OcclusionPlane plane{};
		if (occlude)
		{
			V3 rel = Sub(line.anchor, s_Cam.pos);
			V3 nv = { Dot(line.back, s_Cam.r), Dot(line.back, s_Cam.u), Dot(line.back, s_Cam.f) };
			V3 av = { Dot(rel, s_Cam.r), Dot(rel, s_Cam.u), Dot(rel, s_Cam.f) };
			plane.nx = nv.x;
			plane.ny = nv.y;
			plane.nz = nv.z;
			plane.c = Dot(nv, av);
			float motionMargin = av.z * std::min(0.06f, s_CamTurn * 3.0f);
			plane.margin = line.pinned ? 1e6f : (line.onWall ? 0.3f + av.z * 0.01f : 0.1f + av.z * 0.004f) + motionMargin;
			Occlusion_Begin(dl, plane);
		}
		DrawWorldText(dl, font, line, a);
		if (a.pass != 2)
		{
			if (S.sparkles) { DrawParticles(dl, line, a, S.particleStyle, S.particleAmount * (line.chorus ? 1.8f : 1.0f)); }
			else if (line.chorus) { DrawParticles(dl, line, a, S.particleStyle, 0.8f); }
		}
		if (line.isTitle && line.coverVersion >= 0 && line.coverVersion == Cover_Version() && Cover_View() && a.pass != 2) { DrawCover(dl, font, line, a); }
		if (occlude) { Occlusion_End(dl); }

		if (split)
		{
			a.pass = 2;
			OcclusionPlane raised = plane;
			raised.margin += std::min(maxOff, 0.35f);
			Occlusion_Begin(dl, raised);
			DrawWorldText(dl, font, line, a);
			Occlusion_End(dl);
		}
	}

	s_ProbeCount = checked;
	s_LiftedCount = fixed;
	if (Depth_IsInstalled()) { UpdateDepthChecks(); }

	if (S.showDebug)
	{
		char info[640];
		if (media.available)
		{
			int pm = (int)media.position / 60, ps = (int)media.position % 60;
			int dm = (int)media.duration / 60, ds = (int)media.duration % 60;
			snprintf(info, sizeof(info), ADDON_NAME " " LYRICHUD_VERSION " | %s - %s | %d:%02d / %d:%02d | %s | lyrics: %s (%s) | line %d | on screen %d%s%s",
				media.artist.c_str(), media.title.c_str(), pm, ps, dm, ds, media.playing ? "playing" : "paused",
				StatusName(lyrics.status), lyrics.message.c_str(), s_LastIndex, (int)s_Lines.size(),
				S.beatPulse ? " | beat: " : "", S.beatPulse ? Audio_Status().c_str() : "");
			size_t used = strlen(info);
			snprintf(info + used, sizeof(info) - used, " | sync %+.2fs (%d) | song offset %+.1fs%s", media.syncDrift, media.syncUpdates, SongOffset(s_LastTrackKey), ScriptDebug().c_str());
		}
		else
		{
			snprintf(info, sizeof(info), ADDON_NAME " " LYRICHUD_VERSION " | media: %s", media.error.empty() ? "nothing playing" : media.error.c_str());
		}
		dl->AddText({ 20.0f, 120.0f }, IM_COL32(255, 255, 0, 255), info);
	}
}

static void SetShowLyrics(bool show)
{
	S.showLyrics = show;
	if (!show) { FadeAllLines(ImGui::GetTime()); }
	s_LastIndex = -2;
	MarkDirty(true);
	if (s_API) { s_API->GUI_SendAlert(show ? ADDON_NAME ": on" : ADDON_NAME ": off"); }
}

static void ProcessKeybind(const char* aIdentifier, bool aIsRelease)
{
	if (aIsRelease) { return; }
	if (strcmp(aIdentifier, KeybindToggle) == 0)
	{
		s_ToggleRequested = true;
	}
	else if (strcmp(aIdentifier, KeybindDepth) == 0)
	{
		s_DepthWindowRequested = true;
	}
	else if (strcmp(aIdentifier, KeybindTest) == 0)
	{
		s_TestKeyRequested = true;
	}
	else if (strcmp(aIdentifier, KeybindPlay) == 0) { Media_Command(MediaCommand::PlayPause); }
	else if (strcmp(aIdentifier, KeybindNext) == 0) { Media_Command(MediaCommand::Next); }
	else if (strcmp(aIdentifier, KeybindPrev) == 0) { Media_Command(MediaCommand::Previous); }
	else if (strcmp(aIdentifier, KeybindEarlier) == 0) { s_NudgeRequested += 1; }
	else if (strcmp(aIdentifier, KeybindLater) == 0) { s_NudgeRequested -= 1; }
}

static bool EditColor(const char* label, Color4& c, bool withAlpha)
{
	if (withAlpha) { return ImGui::ColorEdit4(label, c.data(), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar); }
	return ImGui::ColorEdit3(label, c.data(), ImGuiColorEditFlags_NoInputs);
}

static double s_ResetArmedTime = 0.0;

static void ResetSettings()
{
	std::map<std::string, float> measured = S.nearByMap;
	std::map<std::string, float> offsets = S.songOffsets;
	S = Settings();
	S.nearByMap = measured;
	S.songOffsets = offsets;
	s_NearPlane = S.nearPlane;
	s_DepthFailed = false;
	s_LastIndex = -2;
	FadeAllLines(ImGui::GetTime());
	SaveSettings();
	if (s_API) { s_API->GUI_SendAlert(ADDON_NAME ": settings reset to default"); }
}

static void AddonOptions()
{
	ImGui::Text(ADDON_NAME " " LYRICHUD_VERSION);

	if (ImGui::CollapsingHeader("Lyrics", ImGuiTreeNodeFlags_DefaultOpen))
	{
		bool show = S.showLyrics;
		if (ImGui::Checkbox("Show lyrics", &show)) { SetShowLyrics(show); }
		MarkDirty(ImGui::SliderFloat("Lyrics offset (s)", &S.lyricsOffset, -3.0f, 3.0f, "%.2f"));
		if (!s_LastTrackKey.empty())
		{
			float songOffset = SongOffset(s_LastTrackKey);
			if (ImGui::SliderFloat("Offset for this song (s)", &songOffset, -5.0f, 5.0f, "%.2f"))
			{
				if (std::fabs(songOffset) < 0.005f) { S.songOffsets.erase(s_LastTrackKey); }
				else { S.songOffsets[s_LastTrackKey] = songOffset; }
				MarkDirty(true);
			}
			ImGui::SameLine();
			if (ImGui::Button("Reset##songoffset"))
			{
				S.songOffsets.erase(s_LastTrackKey);
				MarkDirty(true);
			}
		}
		MarkDirty(ImGui::SliderFloat("Max time on screen (s)", &S.maxOnScreen, 2.0f, 20.0f, "%.1f"));
		MarkDirty(ImGui::Checkbox("Split long lines", &S.splitLong));
		if (S.splitLong)
		{
			MarkDirty(ImGui::SliderInt("Max letters per piece", &S.maxChars, 10, 60));
		}
		if (ImGui::Button("Test line")) { s_TestRequested = true; }
		ImGui::SameLine();
		if (ImGui::Button("Previous")) { Media_Command(MediaCommand::Previous); }
		ImGui::SameLine();
		if (ImGui::Button("Play / Pause")) { Media_Command(MediaCommand::PlayPause); }
		ImGui::SameLine();
		if (ImGui::Button("Next")) { Media_Command(MediaCommand::Next); }
		MarkDirty(ImGui::Checkbox("Hide during combat", &S.hideInCombat));
		MarkDirty(ImGui::Checkbox("Hide in PvP and WvW", &S.hideInPvP));
		MarkDirty(ImGui::Checkbox("Hide while typing in chat", &S.hideWhileTyping));
		const char* wordModes[] = { "Whole line at once", "Words appear inside the line", "Each word in its own place" };
		MarkDirty(ImGui::Combo("Words", &S.wordMode, wordModes, IM_ARRAYSIZE(wordModes)));
		MarkDirty(ImGui::Checkbox("Spread out repeated words", &S.splitRepeats));
		if (ImGui::IsItemHovered()) { ImGui::SetTooltip("When a word is sung 3 or more times in a row (yeah, yeah, yeah / na-na-na), each one appears in its own spot."); }
		MarkDirty(ImGui::Checkbox("Hide bad words with stars", &S.censor));
		MarkDirty(ImGui::SliderInt("Lyric lines kept on screen", &S.keepLines, 1, 4));
		MarkDirty(ImGui::Checkbox("Backing vocals in (brackets) get their own line", &S.splitBacking));
		if (S.splitBacking)
		{
			ImGui::SameLine();
			MarkDirty(ImGui::Checkbox("In the opposite color", &S.otherVoiceColor));
		}
		MarkDirty(ImGui::Checkbox("Make the chorus stand out", &S.chorusBoost));
		MarkDirty(ImGui::Checkbox("Show the song name when a song starts", &S.titleCard));
		if (S.titleCard)
		{
			ImGui::SameLine();
			MarkDirty(ImGui::Checkbox("With the album cover", &S.coverArt));
		}
		ImGui::TextDisabled("Your own lyrics: put .lrc files named \"Artist - Title.lrc\" in");
		ImGui::TextDisabled("%s", s_LyricsDir.c_str());
	}

	if (ImGui::CollapsingHeader("Placement"))
	{
		if (ImGui::Checkbox("Avoid blocked spots", &S.smartPlacement))
		{
			MarkDirty(true);
			s_DepthFailed = false;
		}
		if (S.smartPlacement)
		{
			MarkDirty(ImGui::Checkbox("Write on walls", &S.useWalls));
			if (S.useWalls)
			{
				MarkDirty(ImGui::SliderFloat("Wall preference", &S.wallChance, 0.0f, 1.0f, "%.2f"));
			}
			MarkDirty(ImGui::Checkbox("Keep away from my character", &S.avoidCharacter));
			MarkDirty(ImGui::Checkbox("Move lines that get blocked", &S.relocate));
		}
		MarkDirty(ImGui::Checkbox("Tilt text toward the camera when looking up or down", &S.tiltToCamera));
		MarkDirty(ImGui::Checkbox("Hide lyrics behind the game's windows (inventory, chat...)", &S.behindUi));
		MarkDirty(ImGui::Checkbox("Put new lines ahead of me when I move", &S.followMovement));
		MarkDirty(ImGui::SliderFloat("Farther away on a mount (x)", &S.mountFarther, 1.0f, 3.0f, "%.1f"));
		MarkDirty(ImGui::Checkbox("Pin lyrics to the screen (they move with the camera)", &S.pinToScreen));
		if (!S.pinToScreen)
		{
			MarkDirty(ImGui::Checkbox("Lyrics turn to face me", &S.faceMe));
		}
		const char* places[] = { "Scattered around the view", "Near the character" };
		MarkDirty(ImGui::Combo("Placement", &S.placeMode, places, IM_ARRAYSIZE(places)));
		if (S.placeMode == Place_Scattered)
		{
			MarkDirty(ImGui::SliderFloat("Nearest (m past character)", &S.aheadMin, -5.0f, 30.0f, "%.1f"));
			MarkDirty(ImGui::SliderFloat("Farthest (m past character)", &S.aheadMax, 0.0f, 60.0f, "%.1f"));
			MarkDirty(ImGui::SliderFloat("Screen side margin", &S.screenMarginX, 0.0f, 0.4f, "%.2f"));
			MarkDirty(ImGui::SliderFloat("Screen top", &S.screenTop, 0.0f, 1.0f, "%.2f"));
			MarkDirty(ImGui::SliderFloat("Screen bottom", &S.screenBottom, 0.0f, 1.0f, "%.2f"));
		}
		else
		{
			MarkDirty(ImGui::SliderFloat("Distance (m)", &S.distance, 1.0f, 20.0f, "%.1f"));
			MarkDirty(ImGui::SliderFloat("Height (m)", &S.height, 0.0f, 6.0f, "%.1f"));
			MarkDirty(ImGui::SliderFloat("Side spread (m)", &S.spreadSide, 0.0f, 5.0f, "%.1f"));
			MarkDirty(ImGui::SliderFloat("Height spread (m)", &S.spreadUp, 0.0f, 3.0f, "%.1f"));
		}
		MarkDirty(ImGui::SliderFloat("World text height (m)", &S.worldHeight, 0.2f, 3.0f, "%.2f"));
		MarkDirty(ImGui::Checkbox("Size from camera distance when it appears", &S.autoScale));
		MarkDirty(ImGui::SliderFloat("Distance where height is exact (m)", &S.refDistance, 2.0f, 30.0f, "%.1f"));
		MarkDirty(ImGui::Checkbox("Readable from behind", &S.readableBack));
	}

	if (ImGui::CollapsingHeader("Look"))
	{
		if (ImGui::BeginCombo("Font", S.fontName.c_str()))
		{
			for (const auto& entry : s_Fonts)
			{
				bool loading = entry.name == S.fontName && !entry.id.empty() && !entry.font;
				std::string label = loading ? entry.name + " (loading)" : entry.name;
				if (ImGui::Selectable(label.c_str(), entry.name == S.fontName))
				{
					S.fontName = entry.name;
					MarkDirty(true);
				}
			}
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Add .ttf or .otf files to %s (new files show up after the addon reloads).", s_FontsDir.c_str());

		MarkDirty(EditColor("Text color", S.textColor, false));
		MarkDirty(ImGui::Checkbox("Take the colors from the album cover", &S.coverColors));
		MarkDirty(ImGui::Checkbox("Particles", &S.sparkles));
		if (S.sparkles)
		{
			const char* styles[] = { "Sparkles", "Confetti", "Embers", "Bubbles", "Snow", "Stars", "Hearts", "Music notes", "Petals", "Fireflies", "Glitter", "Rings", "Autumn leaves", "Butterflies", "Smoke", "Pixels", "Rain" };
			MarkDirty(ImGui::Combo("Particle type", &S.particleStyle, styles, IM_ARRAYSIZE(styles)));
			MarkDirty(ImGui::SliderFloat("Particle amount", &S.particleAmount, 0.3f, 3.0f, "%.1f"));
			MarkDirty(ImGui::Checkbox("Keep particles while the line is on screen", &S.particlesContinuous));
		}
		MarkDirty(ImGui::Checkbox("Pulse with the beat", &S.beatPulse));
		if (S.beatPulse)
		{
			MarkDirty(ImGui::SliderFloat("Pulse strength", &S.beatAmount, 0.02f, 0.25f, "%.2f"));
			ImGui::TextDisabled("Beat: %s", Audio_Status().c_str());
		}
		MarkDirty(ImGui::Checkbox("Light up letters while the line is sung", &S.karaoke));
		if (S.karaoke)
		{
			ImGui::SameLine();
			MarkDirty(EditColor("Sung color", S.karaokeColor, false));
			if (S.coverColors)
			{
				MarkDirty(ImGui::Checkbox("Sung color from the album cover", &S.karaokeFromCover));
			}
		}

		ImGui::Separator();
		MarkDirty(ImGui::Checkbox("3D depth", &S.extrude));
		if (S.extrude)
		{
			ImGui::SameLine();
			MarkDirty(EditColor("##depthcolor", S.depthColor, false));
			MarkDirty(ImGui::Checkbox("Auto layers", &S.autoLayers));
			if (S.autoLayers)
			{
				MarkDirty(ImGui::SliderInt("Max layers", &S.maxLayers, 8, 128));
			}
			else
			{
				MarkDirty(ImGui::SliderInt("Depth layers", &S.extrudeLayers, 1, 64));
			}
			MarkDirty(ImGui::SliderFloat("Depth (x text height)", &S.extrudeDepth, 0.05f, 0.6f, "%.2f"));
		}

		MarkDirty(ImGui::Checkbox("Shadow", &S.shadow));
		if (S.shadow)
		{
			ImGui::SameLine();
			MarkDirty(EditColor("##shadowcolor", S.shadowColor, true));
		}

		MarkDirty(ImGui::Checkbox("Outline", &S.outline));
		if (S.outline)
		{
			ImGui::SameLine();
			MarkDirty(EditColor("##outlinecolor", S.outlineColor, true));
			MarkDirty(ImGui::SliderFloat("Outline width", &S.outlineWidth, 0.01f, 0.12f, "%.2f"));
		}

		if (ImGui::Checkbox("Lift letters out of walls and ground", &S.liftLetters))
		{
			MarkDirty(true);
			s_DepthFailed = false;
		}
		if (ImGui::Checkbox("Hide behind objects", &S.occlusion))
		{
			MarkDirty(true);
			s_DepthFailed = false;
		}
		if (S.occlusion)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(%s, %s)", Depth_GetStatus().c_str(), Occlusion_Status().c_str());
		}

		MarkDirty(ImGui::Checkbox("Glow", &S.glow));
		if (S.glow)
		{
			ImGui::SameLine();
			MarkDirty(EditColor("##glowcolor", S.glowColor, true));
			MarkDirty(ImGui::SliderFloat("Glow size", &S.glowSize, 0.05f, 0.5f, "%.2f"));
		}
	}

	if (ImGui::CollapsingHeader("Animation"))
	{
		const char* appear[] = { "Fade", "Fade and rise", "Letter by letter", "Scatter in", "Bounce", "Spin", "Pop", "Drop in", "Zoom in", "Typewriter", "Wave in", "Glitch in", "Come from behind", "Slide in", "Flip", "Elastic", "Rain down", "Swirl" };
		MarkDirty(ImGui::Combo("Appear", &S.appearMode, appear, IM_ARRAYSIZE(appear)));
		MarkDirty(ImGui::SliderFloat("Appear time (s)", &S.appearTime, 0.1f, 2.0f, "%.2f"));
		if (S.appearMode != Appear_Fade && S.appearMode != Appear_FadeRise && S.appearMode != Appear_Zoom)
		{
			MarkDirty(ImGui::SliderFloat("Letter delay (s)", &S.letterDelay, 0.0f, 0.2f, "%.3f"));
		}
		const char* lands[] = { "None", "Flash", "Ring", "Sparks", "Bounce" };
		MarkDirty(ImGui::Combo("When a letter lands", &S.landEffect, lands, IM_ARRAYSIZE(lands)));

		const char* idle[] = { "None", "Float", "Wave", "Pulse", "Shake", "Swing", "Jelly", "Glitch" };
		MarkDirty(ImGui::Combo("While on screen", &S.idleMode, idle, IM_ARRAYSIZE(idle)));
		if (S.idleMode != Idle_None)
		{
			MarkDirty(ImGui::SliderFloat("Motion strength", &S.idleStrength, 0.2f, 3.0f, "%.1f"));
		}

		const char* disappear[] = { "Fade up", "Fade", "Scatter out", "Shrink", "Drop", "Spin out", "Blow away", "Burst", "Letter by letter", "Sink", "Slide out", "Flip out", "Melt", "Shatter", "Swirl out" };
		MarkDirty(ImGui::Combo("Disappear", &S.disappearMode, disappear, IM_ARRAYSIZE(disappear)));
		MarkDirty(ImGui::SliderFloat("Disappear time (s)", &S.disappearTime, 0.1f, 2.0f, "%.2f"));

		if (ImGui::Button("Preview")) { s_TestRequested = true; }
	}

	if (ImGui::CollapsingHeader("Debug"))
	{
		MarkDirty(ImGui::Checkbox("Show debug info", &S.showDebug));
		ImGui::Checkbox("Depth test window", &s_DepthWindow);
		MarkDirty(ImGui::SliderFloat("Depth near plane (m)", &S.nearPlane, 0.2f, 0.8f, "%.3f"));
		if (ImGui::Button("Reset near plane")) { S.nearPlane = 0.288f; MarkDirty(true); }
		MarkDirty(ImGui::Checkbox("Measure depth scale automatically", &S.autoNearPlane));
		ImGui::TextDisabled("Measured for this map: %.4f m | in use: %.4f m", s_MeasuredNear, s_NearPlane);
		ImGui::TextDisabled("Depth: %s | near plane %.3f m", Depth_GetStatus().c_str(), s_NearPlane);
		ImGui::TextDisabled("Settings file: %s", s_SettingsPath.c_str());
	}

	ImGui::Separator();
	if (s_ResetArmedTime > 0.0 && ImGui::GetTime() - s_ResetArmedTime > 4.0) { s_ResetArmedTime = 0.0; }
	if (s_ResetArmedTime <= 0.0)
	{
		if (ImGui::Button("Reset all settings to default")) { s_ResetArmedTime = ImGui::GetTime(); }
	}
	else
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.2f, 0.2f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.25f, 0.25f, 1.0f));
		bool confirm = ImGui::Button("Click again to reset everything");
		ImGui::PopStyleColor(2);
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) { s_ResetArmedTime = 0.0; }
		if (confirm)
		{
			ResetSettings();
			s_ResetArmedTime = 0.0;
		}
	}
}

static void AddFontEntry(const std::string& name, const std::string& path)
{
	if (s_Fonts.size() >= 64) { return; }
	if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) { return; }
	for (const auto& e : s_Fonts)
	{
		if (e.name == name) { return; }
	}
	FontEntry entry;
	entry.name = name;
	entry.path = path;
	entry.id = "LYRICHUD_FONT_" + std::to_string(s_Fonts.size());
	s_Fonts.push_back(entry);
}

static void LoadFonts()
{
	s_Fonts.clear();
	FontEntry game;
	game.name = GameFontName;
	s_Fonts.push_back(game);

	char windir[MAX_PATH];
	UINT len = GetWindowsDirectoryA(windir, MAX_PATH);
	if (len > 0 && len < MAX_PATH - 40)
	{
		std::string fonts = std::string(windir) + "\\Fonts\\";
		AddFontEntry("Segoe UI Bold", fonts + "segoeuib.ttf");
		AddFontEntry("Impact", fonts + "impact.ttf");
	}

	if (!s_FontsDir.empty())
	{
		const char* patterns[] = { "*.ttf", "*.otf" };
		for (const char* pattern : patterns)
		{
			WIN32_FIND_DATAA data;
			HANDLE find = FindFirstFileA((s_FontsDir + "\\" + pattern).c_str(), &data);
			if (find == INVALID_HANDLE_VALUE) { continue; }
			do
			{
				if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { continue; }
				std::string file = data.cFileName;
				std::string name = file.substr(0, file.find_last_of('.'));
				AddFontEntry(name, s_FontsDir + "\\" + file);
			} while (FindNextFileA(find, &data));
			FindClose(find);
		}
	}

	s_FontConfig = ImFontConfig();
	s_FontConfig.GlyphRanges = FontRanges;
	s_FontConfig.OversampleH = 2;
	s_FontConfig.OversampleV = 1;

}

static void EnsureFontLoaded()
{
	for (auto& entry : s_Fonts)
	{
		if (entry.id.empty()) { continue; }
		bool wanted = entry.name == S.fontName;
		if (wanted && !entry.requested)
		{
			entry.requested = true;
			s_API->Fonts_AddFromFile(entry.id.c_str(), 96.0f, entry.path.c_str(), ReceiveFont, &s_FontConfig);
		}
		else if (!wanted && entry.requested)
		{
			entry.requested = false;
			entry.font = nullptr;
			s_API->Fonts_Release(entry.id.c_str(), ReceiveFont);
		}
	}
}

static void ReleaseFonts()
{
	for (auto& entry : s_Fonts)
	{
		if (entry.id.empty() || !entry.requested) { continue; }
		s_API->Fonts_Release(entry.id.c_str(), ReceiveFont);
		entry.font = nullptr;
		entry.requested = false;
	}
	s_Fonts.clear();
	ReleaseScriptFont();
	s_ScriptSource = nullptr;
	s_ScriptTitle.clear();
}

#ifdef LYRICHUD_RELEASE
static void InstallBundledFonts()
{
	if (s_FontsDir.empty()) { return; }
	std::string marker = s_FontsDir + "\\.bundled";
	std::string stamp = LYRICHUD_VERSION;
	{
		std::ifstream in(marker);
		std::string have;
		std::getline(in, have);
		if (have == stamp) { return; }
	}
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&InstallBundledFonts, &self);
	if (!self) { return; }
	for (const auto& f : BundledFonts)
	{
		std::string path = s_FontsDir + "\\" + f.file;
		if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) { continue; }
		HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(f.id), MAKEINTRESOURCEA(10));
		if (!res) { continue; }
		HGLOBAL data = LoadResource(self, res);
		DWORD size = SizeofResource(self, res);
		const void* bytes = data ? LockResource(data) : nullptr;
		if (!bytes || size == 0) { continue; }
		std::ofstream out(path, std::ios::binary);
		out.write((const char*)bytes, size);
	}
	std::ofstream(marker) << stamp;
}
#endif

static void AddonLoad(AddonAPI_t* aApi)
{
	s_API = aApi;

	ImGui::SetCurrentContext((ImGuiContext*)s_API->ImguiContext);
	ImGui::SetAllocatorFunctions(
		(void* (*)(size_t, void*))s_API->ImguiMalloc,
		(void (*)(void*, void*))s_API->ImguiFree
	);

	s_Nexus    = (NexusLinkData_t*)s_API->DataLink_Get(DL_NEXUS_LINK);
	s_Mumble   = (Mumble::Data*)s_API->DataLink_Get(DL_MUMBLE_LINK);
	s_Identity = (Mumble::Identity*)s_API->DataLink_Get(DL_MUMBLE_LINK_IDENTITY);

	const char* dir = s_API->Paths_GetAddonDirectory(ADDON_FOLDER);
	if (dir)
	{
		s_AddonDir = dir;
		CreateDirectoryA(s_AddonDir.c_str(), nullptr);
		s_SettingsPath = s_AddonDir + "\\settings.json";
#ifdef LYRICHUD_RELEASE
		if (GetFileAttributesA(s_SettingsPath.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			const char* old = s_API->Paths_GetAddonDirectory("LyricHud");
			if (old)
			{
				std::string oldDir = old;
				CopyFileA((oldDir + "\\settings.json").c_str(), s_SettingsPath.c_str(), TRUE);
				CreateDirectoryA((s_AddonDir + "\\lyrics").c_str(), nullptr);
				WIN32_FIND_DATAA found;
				HANDLE h = FindFirstFileA((oldDir + "\\lyrics\\*.lrc").c_str(), &found);
				if (h != INVALID_HANDLE_VALUE)
				{
					do { CopyFileA((oldDir + "\\lyrics\\" + found.cFileName).c_str(), (s_AddonDir + "\\lyrics\\" + found.cFileName).c_str(), TRUE); } while (FindNextFileA(h, &found));
					FindClose(h);
				}
			}
		}
#endif
		s_FontsDir = s_AddonDir + "\\fonts";
		CreateDirectoryA(s_FontsDir.c_str(), nullptr);
#ifdef LYRICHUD_RELEASE
		InstallBundledFonts();
#endif
		s_LyricsDir = s_AddonDir + "\\lyrics";
		CreateDirectoryA(s_LyricsDir.c_str(), nullptr);
		Lyrics_SetFolder(s_LyricsDir);
		LoadSettings();
	}
	s_LiveText = S.textColor;
	s_LiveDepth = S.depthColor;

	s_Lines.clear();
	s_Expanded.clear();
	s_ExpandedSource = nullptr;
	ResetGrid();
	s_LastTrackKey.clear();
	s_LastIndex = -2;
	s_Rng ^= GetTickCount();

	LoadFonts();

	s_API->Textures_LoadFromMemory(IconId, (void*)IconPng, sizeof(IconPng), ReceiveTexture);
	s_API->Textures_LoadFromMemory(IconHoverId, (void*)IconHoverPng, sizeof(IconHoverPng), ReceiveTexture);
	s_API->InputBinds_RegisterWithString(KeybindToggle, ProcessKeybind, "ALT+SHIFT+L");
	s_API->InputBinds_RegisterWithString(KeybindDepth, ProcessKeybind, "ALT+SHIFT+D");
	s_API->InputBinds_RegisterWithString(KeybindTest, ProcessKeybind, "ALT+SHIFT+F8");
	s_API->InputBinds_RegisterWithString(KeybindPrev, ProcessKeybind, "ALT+SHIFT+8");
	s_API->InputBinds_RegisterWithString(KeybindPlay, ProcessKeybind, "ALT+SHIFT+9");
	s_API->InputBinds_RegisterWithString(KeybindNext, ProcessKeybind, "ALT+SHIFT+0");
	s_API->InputBinds_RegisterWithString(KeybindEarlier, ProcessKeybind, "ALT+SHIFT+K");
	s_API->InputBinds_RegisterWithString(KeybindLater, ProcessKeybind, "ALT+SHIFT+J");
	s_API->QuickAccess_Add(ShortcutId, IconId, IconHoverId, KeybindToggle, ADDON_NAME);

	Media_Start();
	Lyrics_Start();

	s_API->Log(LOGL_INFO, "LyricHud", "Loaded " ADDON_NAME " " LYRICHUD_VERSION ".");
}

static void AddonUnload()
{
	if (s_Dirty) { SaveSettings(); }

	s_API->QuickAccess_Remove(ShortcutId);
	s_API->InputBinds_Deregister(KeybindToggle);
	s_API->InputBinds_Deregister(KeybindDepth);
	s_API->InputBinds_Deregister(KeybindTest);
	s_API->InputBinds_Deregister(KeybindPrev);
	s_API->InputBinds_Deregister(KeybindPlay);
	s_API->InputBinds_Deregister(KeybindNext);
	s_API->InputBinds_Deregister(KeybindEarlier);
	s_API->InputBinds_Deregister(KeybindLater);

	Occlusion_Shutdown();
	Audio_Stop();
	Cover_Release();
	Depth_Uninstall();
	Lyrics_Stop();
	Media_Stop();

	ReleaseFonts();
	s_Lines.clear();
	s_Expanded.clear();
	s_Nexus = nullptr;
	s_Mumble = nullptr;
	s_Identity = nullptr;
	s_API = nullptr;
}

extern "C" __declspec(dllexport) void CoreLoad(AddonAPI_t* aApi)
{
	AddonLoad(aApi);
}

extern "C" __declspec(dllexport) void CoreUnload()
{
	AddonUnload();
}

extern "C" __declspec(dllexport) void CoreRender()
{
	AddonRender();
}

extern "C" __declspec(dllexport) void CoreOptions()
{
	AddonOptions();
}

#ifdef LYRICHUD_RELEASE
#ifndef LYRICHUD_REPO
#define LYRICHUD_REPO "https://github.com/3oXYLY/Echoes_of_Tyria"
#endif

static void ReleaseLoad(AddonAPI_t* aApi)
{
	AddonLoad(aApi);
	s_API->GUI_Register(RT_Render, AddonRender);
	s_API->GUI_Register(RT_OptionsRender, AddonOptions);
}

static void ReleaseUnload()
{
	s_API->GUI_Deregister(AddonOptions);
	s_API->GUI_Deregister(AddonRender);
	AddonUnload();
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
	static AddonDefinition_t def{};
	def.Signature        = 0x7A3F19C2;
	def.APIVersion       = NEXUS_API_VERSION;
	def.Name             = ADDON_NAME;
	def.Version.Major    = 1;
	def.Version.Minor    = 0;
	def.Version.Build    = 0;
	def.Version.Revision = 0;
	def.Author           = "Foxy";
	def.Description      = "Shows the synced lyrics of the song you are playing as 3D text in the game world.";
	def.Load             = ReleaseLoad;
	def.Unload           = ReleaseUnload;
	def.Flags            = AF_None;
	def.Provider         = LYRICHUD_REPO[0] ? UP_GitHub : UP_None;
	def.UpdateLink       = LYRICHUD_REPO[0] ? LYRICHUD_REPO : nullptr;
	return &def;
}
#endif

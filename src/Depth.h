#pragma once

#include <string>
#include <vector>

struct DepthCandidate
{
	unsigned width;
	unsigned height;
	unsigned format;
	unsigned samples;
	int      draws;
	int      views;
	bool     chosen;
};

struct DepthSample
{
	float screenX;
	float screenY;
	float expectedZ;
	float raw;
	bool  valid;
	int   tag;
	int   index;
};

struct DepthStats
{
	int  copiesLastSecond;
	int  framesSinceCopy;
	int  immediateDraws;
	int  otherDraws;
	int  commandLists;
	int  clears;
	int  trackedViews;
	int  trackedTextures;
	int  switches;
};

struct DepthPass
{
	unsigned width;
	unsigned height;
	unsigned format;
	bool     backBuffer;
	bool     depth;
	int      targets;
	int      draws;
	unsigned minCount;
	unsigned maxCount;
	int      smallDraws;
	void*    texture;
};

std::vector<DepthPass> Depth_GetPasses();
std::string Depth_HookReport();
void Depth_SetUiMaskEnabled(bool enabled);
bool Depth_GetUiMask(void*& before, void*& after);
std::string Depth_UiStatus();
void Depth_RequestDump(const std::string& folder);
void Depth_SetDiagnostics(bool enabled);

bool Depth_Install(void* swapChain);
void Depth_Uninstall();
bool Depth_IsInstalled();
void Depth_ResetTracking();

void Depth_EndFrame(float screenWidth, float screenHeight, int batch);
bool Depth_GetGrid(const float*& data, int& width, int& height, int& batch);
std::string Depth_GridStatus();
void Depth_RequestSamples(const std::vector<DepthSample>& samples);
std::vector<DepthSample> Depth_GetSamples();

std::vector<DepthCandidate> Depth_GetCandidates();
DepthStats Depth_GetStats();
void* Depth_GetPreview(unsigned& width, unsigned& height);
void* Depth_GetDevice();
void* Depth_GetContext();
std::string Depth_GetStatus();

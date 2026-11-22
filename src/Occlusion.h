#pragma once

#include <string>

struct ImDrawList;

struct OcclusionPlane
{
	float nx;
	float ny;
	float nz;
	float c;
	float margin;
};

void Occlusion_BeginFrame(float tanHalf, float aspect, float screenW, float screenH, float nearPlane, float softness, float holeX1, float holeY1, float holeX2, float holeY2);
void Occlusion_SetDepthK(float k);
bool Occlusion_Ready();
void Occlusion_Begin(ImDrawList* dl, const OcclusionPlane& plane);
void Occlusion_End(ImDrawList* dl);
void Occlusion_Shutdown();
std::string Occlusion_Status();

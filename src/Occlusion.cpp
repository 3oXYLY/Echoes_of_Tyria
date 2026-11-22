#include "Occlusion.h"
#include "Depth.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcommon.h>

#include <cstdint>
#include <cstring>

#include "imgui.h"

typedef HRESULT (WINAPI *PFN_D3DCompileFunc)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static const char* ShaderSource = R"(
struct PS_INPUT
{
	float4 pos : SV_POSITION;
	float4 col : COLOR0;
	float2 uv  : TEXCOORD0;
};

sampler sampler0 : register(s0);
Texture2D texture0 : register(t0);
Texture2D<float> depthTex : register(t1);
Texture2D uiBefore : register(t2);
Texture2D uiAfter : register(t3);

cbuffer Occlusion : register(b0)
{
	float4 plane;
	float4 proj;
	float4 info;
	float4 info2;
	float4 hole;
	float4 ui;
};

float UiDiff(float2 p)
{
	int2 q = int2(clamp(p.x, 0.0, ui.z), clamp(p.y, 0.0, ui.w));
	float3 d = abs(uiBefore.Load(int3(q, 0)).rgb - uiAfter.Load(int3(q, 0)).rgb);
	return max(d.r, max(d.g, d.b));
}

float4 main(PS_INPUT input) : SV_Target
{
	float4 c = input.col * texture0.Sample(sampler0, input.uv);
	float2 ndc = float2(input.pos.x / proj.z * 2.0 - 1.0, 1.0 - input.pos.y / proj.w * 2.0);
	float3 dir = float3(ndc.x * proj.x, ndc.y * proj.y, 1.0);
	float denom = dot(plane.xyz, dir);
	float z = abs(denom) > 1e-6 ? plane.w / denom : 1e9;
	int2 dp = int2(clamp(input.pos.x * info.y, 0.0, info2.x), clamp(input.pos.y * info.z, 0.0, info2.y));
	float raw = depthTex.Load(int3(dp, 0));
	float scene = raw >= 0.99999 ? 1e9 : info.x / max(1e-6, 1.0 - raw + info2.w);
	float vis = saturate((scene - z + info.w) / info2.z);
	if (hole.z > hole.x && input.pos.x >= hole.x && input.pos.x <= hole.z && input.pos.y >= hole.y && input.pos.y <= hole.w) { vis = 0.0; }
	if (ui.x > 0.5)
	{
		float2 p = input.pos.xy;
		float hide = saturate((UiDiff(p) - ui.y) / 0.02);
		[branch] if (hide < 1.0)
		{
			float px = (ui.w + 1.0) / 1080.0;
			int pairs = 0;
			[loop] for (int k = 0; k < 4; k++)
			{
				float2 dir = k == 0 ? float2(1.0, 0.0) : (k == 1 ? float2(0.0, 1.0) : (k == 2 ? float2(0.7071, 0.7071) : float2(0.7071, -0.7071)));
				dir *= px;
				float a = max(UiDiff(p + dir * 16.0), max(UiDiff(p + dir * 48.0), UiDiff(p + dir * 120.0)));
				if (a > 0.15)
				{
					float b = max(UiDiff(p - dir * 16.0), max(UiDiff(p - dir * 48.0), UiDiff(p - dir * 120.0)));
					if (b > 0.15) { pairs++; }
				}
				if (pairs >= 2 || pairs + (3 - k) < 2) { break; }
			}
			if (pairs >= 2) { hide = 1.0; }
		}
		vis *= 1.0 - hide;
	}
	c.a *= vis;
	return c;
}
)";

struct PlaneData
{
	float plane[4];
	float margin;
};

struct alignas(16) ConstantData
{
	float plane[4];
	float proj[4];
	float info[4];
	float info2[4];
	float hole[4];
	float ui[4];
};

static const int MaxPlanes = 64;
static PlaneData s_Planes[MaxPlanes];
static int       s_PlaneCount = 0;

static ID3D11PixelShader* s_Shader = nullptr;
static ID3D11Buffer*      s_Buffer = nullptr;
static ID3D11Device*      s_ForDevice = nullptr;
static ID3D11PixelShader* s_SavedShader = nullptr;
static bool               s_Active = false;
static bool               s_Failed = false;
static std::string        s_Status = "not started";

static float s_TanHalf = 1.0f;
static float s_Aspect = 1.0f;
static float s_ScreenW = 1.0f;
static float s_ScreenH = 1.0f;
static float s_Near = 0.288f;
static float s_DepthK = 0.0f;
static float s_Softness = 0.2f;
static float s_Hole[4] = { 0, 0, 0, 0 };

static bool CreateResources()
{
	ID3D11Device* device = (ID3D11Device*)Depth_GetDevice();
	if (!device) { return false; }
	if (s_Shader && s_ForDevice == device) { return true; }
	if (s_Failed) { return false; }

	HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
	if (!compiler) { s_Failed = true; s_Status = "d3dcompiler_47.dll not found"; return false; }
	PFN_D3DCompileFunc compile = (PFN_D3DCompileFunc)GetProcAddress(compiler, "D3DCompile");
	if (!compile) { s_Failed = true; s_Status = "D3DCompile not found"; return false; }

	ID3DBlob* code = nullptr;
	ID3DBlob* errors = nullptr;
	HRESULT hr = compile(ShaderSource, strlen(ShaderSource), "LyricHudOcclusion", nullptr, nullptr, "main", "ps_4_0", 0, 0, &code, &errors);
	if (FAILED(hr) || !code)
	{
		s_Status = "shader compile failed";
		if (errors)
		{
			s_Status += ": ";
			s_Status += std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
			errors->Release();
		}
		s_Failed = true;
		return false;
	}
	if (errors) { errors->Release(); }

	hr = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s_Shader);
	code->Release();
	if (FAILED(hr)) { s_Shader = nullptr; s_Failed = true; s_Status = "shader create failed"; return false; }

	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = sizeof(ConstantData);
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(device->CreateBuffer(&desc, nullptr, &s_Buffer)))
	{
		s_Shader->Release();
		s_Shader = nullptr;
		s_Buffer = nullptr;
		s_Failed = true;
		s_Status = "buffer create failed";
		return false;
	}
	s_ForDevice = device;
	s_Status = "ready";
	return true;
}

static void CallbackBegin(const ImDrawList*, const ImDrawCmd* cmd)
{
	int index = (int)(intptr_t)cmd->UserCallbackData;
	if (index < 0 || index >= s_PlaneCount || !s_Shader || !s_Buffer) { return; }
	ID3D11DeviceContext* ctx = (ID3D11DeviceContext*)Depth_GetContext();
	unsigned dw = 0, dh = 0;
	ID3D11ShaderResourceView* srv = (ID3D11ShaderResourceView*)Depth_GetPreview(dw, dh);
	if (!ctx || !srv || dw == 0 || dh == 0) { return; }

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(ctx->Map(s_Buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) { return; }
	ConstantData data{};
	const PlaneData& p = s_Planes[index];
	memcpy(data.plane, p.plane, sizeof(data.plane));
	data.proj[0] = s_TanHalf * s_Aspect;
	data.proj[1] = s_TanHalf;
	data.proj[2] = s_ScreenW;
	data.proj[3] = s_ScreenH;
	data.info[0] = s_Near;
	data.info[1] = dw / s_ScreenW;
	data.info[2] = dh / s_ScreenH;
	data.info[3] = p.margin;
	data.info2[0] = (float)dw - 1.0f;
	data.info2[1] = (float)dh - 1.0f;
	data.info2[2] = s_Softness;
	data.info2[3] = s_DepthK;
	memcpy(data.hole, s_Hole, sizeof(data.hole));
	void* uiBefore = nullptr;
	void* uiAfter = nullptr;
	bool useUi = Depth_GetUiMask(uiBefore, uiAfter);
	data.ui[0] = useUi ? 1.0f : 0.0f;
	data.ui[1] = 0.025f;
	data.ui[2] = s_ScreenW - 1.0f;
	data.ui[3] = s_ScreenH - 1.0f;
	memcpy(mapped.pData, &data, sizeof(data));
	ctx->Unmap(s_Buffer, 0);

	if (!s_SavedShader) { ctx->PSGetShader(&s_SavedShader, nullptr, nullptr); }
	ctx->PSSetShader(s_Shader, nullptr, 0);
	ctx->PSSetShaderResources(1, 1, &srv);
	if (useUi)
	{
		ID3D11ShaderResourceView* views[2] = { (ID3D11ShaderResourceView*)uiBefore, (ID3D11ShaderResourceView*)uiAfter };
		ctx->PSSetShaderResources(2, 2, views);
	}
	ctx->PSSetConstantBuffers(0, 1, &s_Buffer);
	s_Active = true;
}

static void CallbackEnd(const ImDrawList*, const ImDrawCmd*)
{
	if (!s_Active) { return; }
	ID3D11DeviceContext* ctx = (ID3D11DeviceContext*)Depth_GetContext();
	if (!ctx) { return; }
	ctx->PSSetShader(s_SavedShader, nullptr, 0);
	if (s_SavedShader) { s_SavedShader->Release(); s_SavedShader = nullptr; }
	ID3D11ShaderResourceView* none[3] = { nullptr, nullptr, nullptr };
	ctx->PSSetShaderResources(1, 3, none);
	ID3D11Buffer* noBuffer = nullptr;
	ctx->PSSetConstantBuffers(0, 1, &noBuffer);
	s_Active = false;
}

void Occlusion_BeginFrame(float tanHalf, float aspect, float screenW, float screenH, float nearPlane, float softness, float holeX1, float holeY1, float holeX2, float holeY2)
{
	s_Hole[0] = holeX1;
	s_Hole[1] = holeY1;
	s_Hole[2] = holeX2;
	s_Hole[3] = holeY2;
	s_PlaneCount = 0;
	s_TanHalf = tanHalf;
	s_Aspect = aspect;
	s_ScreenW = screenW > 1.0f ? screenW : 1.0f;
	s_ScreenH = screenH > 1.0f ? screenH : 1.0f;
	s_Near = nearPlane;
	s_Softness = softness > 0.01f ? softness : 0.01f;
	if (Depth_IsInstalled()) { CreateResources(); }
}

void Occlusion_SetDepthK(float k)
{
	s_DepthK = k;
}

bool Occlusion_Ready()
{
	unsigned w = 0, h = 0;
	return Depth_IsInstalled() && s_Shader && s_Buffer && Depth_GetPreview(w, h) != nullptr;
}

void Occlusion_Begin(ImDrawList* dl, const OcclusionPlane& plane)
{
	if (!Occlusion_Ready() || s_PlaneCount >= MaxPlanes) { return; }
	PlaneData& p = s_Planes[s_PlaneCount];
	p.plane[0] = plane.nx;
	p.plane[1] = plane.ny;
	p.plane[2] = plane.nz;
	p.plane[3] = plane.c;
	p.margin = plane.margin;
	dl->AddCallback(CallbackBegin, (void*)(intptr_t)s_PlaneCount);
	s_PlaneCount++;
}

void Occlusion_End(ImDrawList* dl)
{
	if (!Occlusion_Ready()) { return; }
	dl->AddCallback(CallbackEnd, nullptr);
}

void Occlusion_Shutdown()
{
	if (s_SavedShader) { s_SavedShader->Release(); s_SavedShader = nullptr; }
	if (s_Buffer) { s_Buffer->Release(); s_Buffer = nullptr; }
	if (s_Shader) { s_Shader->Release(); s_Shader = nullptr; }
	s_ForDevice = nullptr;
	s_PlaneCount = 0;
	s_Failed = false;
	s_Status = "not started";
}

std::string Occlusion_Status()
{
	return s_Status;
}

#include "Depth.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcommon.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <unordered_map>
#include <set>

typedef void (STDMETHODCALLTYPE *PFN_OMSetRenderTargets)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void (STDMETHODCALLTYPE *PFN_OMSetRenderTargetsAndUAVs)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
typedef void (STDMETHODCALLTYPE *PFN_ClearDepthStencilView)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
typedef void (STDMETHODCALLTYPE *PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void (STDMETHODCALLTYPE *PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void (STDMETHODCALLTYPE *PFN_DrawIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE *PFN_DrawInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE *PFN_ExecuteCommandList)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
typedef void (STDMETHODCALLTYPE *PFN_CopyResource)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void (STDMETHODCALLTYPE *PFN_ResolveSubresource)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);

enum VtableIndex
{
	VT_DrawIndexed          = 12,
	VT_Draw                 = 13,
	VT_DrawIndexedInstanced = 20,
	VT_DrawInstanced        = 21,
	VT_OMSetRenderTargets   = 33,
	VT_OMSetRTsAndUAVs      = 34,
	VT_ClearDepthStencil    = 53,
	VT_ExecuteCommandList   = 58
};

struct TexStats
{
	D3D11_TEXTURE2D_DESC desc{};
	int drawsThisFrame = 0;
	int drawsLastFrame = 0;
	int drawsSinceClear = 0;
	int viewsThisFrame = 0;
	int viewsLastFrame = 0;
	int idleFrames = 0;
};

struct ViewInfo
{
	ID3D11Texture2D* texture = nullptr;
	int idleFrames = 0;
	bool usedThisFrame = false;
};

struct ContextSlot
{
	ID3D11DeviceContext* ctx = nullptr;
	TexStats* current = nullptr;
};

static ID3D11Device*        s_Device  = nullptr;
static ID3D11DeviceContext* s_Context = nullptr;
static void**               s_Vtable  = nullptr;
static bool                 s_Installed = false;

static PFN_OMSetRenderTargets         o_OMSetRenderTargets = nullptr;
static PFN_OMSetRenderTargetsAndUAVs  o_OMSetRTsAndUAVs = nullptr;
static PFN_ClearDepthStencilView      o_ClearDepthStencilView = nullptr;
static PFN_DrawIndexed                o_DrawIndexed = nullptr;
static PFN_Draw                       o_Draw = nullptr;
static PFN_DrawIndexedInstanced       o_DrawIndexedInstanced = nullptr;
static PFN_DrawInstanced              o_DrawInstanced = nullptr;
static PFN_ExecuteCommandList         o_ExecuteCommandList = nullptr;
static PFN_CopyResource               o_CopyResource = nullptr;
static PFN_ResolveSubresource         o_ResolveSubresource = nullptr;

static std::map<ID3D11DepthStencilView*, ViewInfo> s_Views;
static std::map<ID3D11Texture2D*, TexStats>        s_Textures;
static ContextSlot      s_Slots[16];
static ID3D11Texture2D* s_Chosen = nullptr;
static ID3D11Texture2D* s_Challenger = nullptr;
static int              s_ChallengeFrames = 0;
static bool             s_InCopy = false;
static int              s_Switches = 0;

static ID3D11Texture2D*          s_Copy = nullptr;
static ID3D11ShaderResourceView* s_CopySrv = nullptr;
static D3D11_TEXTURE2D_DESC      s_CopyDesc{};
static bool                      s_HasCopy = false;

static const int RingSize = 3;
static const int MaxSamples = 64;
static ID3D11Texture2D*         s_Staging[RingSize] = {};
static DXGI_FORMAT              s_StagingFormat = DXGI_FORMAT_UNKNOWN;
static std::vector<DepthSample> s_Pending[RingSize];
static bool                     s_Used[RingSize] = {};
static int                      s_RingIndex = 0;
static std::vector<DepthSample> s_Results;
static std::string              s_Status = "not installed";
static float                    s_ScreenW = 1.0f;
static float                    s_ScreenH = 1.0f;

static int   s_ImmediateDraws = 0;
struct RtInfo
{
	void*    texture = nullptr;
	unsigned width = 0;
	unsigned height = 0;
	unsigned format = 0;
};
static std::unordered_map<ID3D11RenderTargetView*, RtInfo> s_RtCache;
static std::vector<DepthPass> s_PassLog;
static std::vector<DepthPass> s_LastPasses;
static IDXGISwapChain*        s_SwapChain = nullptr;
static void*                  s_BackBufferPtr = nullptr;
static int                    s_PassFrames = 0;
static bool                   s_UiEnabled = true;
static int                    s_UiPassIndex = -1;
static void*                  s_UiTexture = nullptr;
static ID3D11Texture2D*       s_UiHeld = nullptr;
static ID3D11Texture2D*       s_UiBefore = nullptr;
static ID3D11Texture2D*       s_UiAfter = nullptr;
static ID3D11ShaderResourceView* s_UiBeforeSrv = nullptr;
static ID3D11ShaderResourceView* s_UiAfterSrv = nullptr;
static D3D11_TEXTURE2D_DESC   s_UiDesc{};
static bool                   s_UiReady = false;
static int                    s_UiMisses = 0;
static bool                   s_InUiCopy = false;
static bool                   s_UiArmed = false;
static int                    s_UiArmDraws = 0;
static bool                   s_UiCaptured = false;
static ID3D11Texture2D*       s_UiProbe[2] = { nullptr, nullptr };
static int                    s_UiProbeTick = 0;
static std::string            s_UiProbeText = "no samples yet";

static std::string s_DumpFolder;
static bool        s_DumpRequested = false;
static std::string s_DumpResult;

static void WriteBmp(const std::string& path, ID3D11Texture2D* source)
{
	if (!source) { return; }
	D3D11_TEXTURE2D_DESC desc;
	source->GetDesc(&desc);
	bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
	bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
	if ((!rgba && !bgra) || desc.SampleDesc.Count > 1) { s_DumpResult += " skip fmt" + std::to_string((int)desc.Format); return; }
	D3D11_TEXTURE2D_DESC sd = desc;
	sd.MipLevels = 1;
	sd.ArraySize = 1;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	ID3D11Texture2D* staging = nullptr;
	if (FAILED(s_Device->CreateTexture2D(&sd, nullptr, &staging)) || !staging) { return; }
	s_InCopy = true;
	o_CopyResource(s_Context, staging, source);
	s_InCopy = false;
	D3D11_MAPPED_SUBRESOURCE m{};
	if (SUCCEEDED(s_Context->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
	{
		unsigned w = desc.Width / 2, h = desc.Height / 2;
		unsigned row = (w * 4 + 3) & ~3u;
		std::vector<unsigned char> file(54 + row * h * 2, 0);
		std::vector<unsigned char> pixels(row * h, 0);
		std::vector<unsigned char> alpha(row * h, 0);
		for (unsigned y = 0; y < h; y++)
		{
			const unsigned char* src = (const unsigned char*)m.pData + (size_t)(y * 2) * m.RowPitch;
			unsigned char* dst = pixels.data() + (size_t)(h - 1 - y) * row;
			unsigned char* adst = alpha.data() + (size_t)(h - 1 - y) * row;
			for (unsigned x = 0; x < w; x++)
			{
				const unsigned char* p = src + x * 8;
				dst[x * 4 + 0] = bgra ? p[0] : p[2];
				dst[x * 4 + 1] = p[1];
				dst[x * 4 + 2] = bgra ? p[2] : p[0];
				dst[x * 4 + 3] = 255;
				adst[x * 4 + 0] = adst[x * 4 + 1] = adst[x * 4 + 2] = p[3];
				adst[x * 4 + 3] = 255;
			}
		}
		auto writeOne = [&](const std::string& file, const std::vector<unsigned char>& data)
		{
			unsigned size = 54 + row * h;
			unsigned char header[54] = { 'B', 'M' };
			memcpy(header + 2, &size, 4);
			unsigned offset = 54, info = 40, planes = 1 | (32 << 16);
			memcpy(header + 10, &offset, 4);
			memcpy(header + 14, &info, 4);
			memcpy(header + 18, &w, 4);
			memcpy(header + 22, &h, 4);
			memcpy(header + 26, &planes, 4);
			HANDLE out = CreateFileA(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (out == INVALID_HANDLE_VALUE) { return; }
			DWORD written = 0;
			WriteFile(out, header, 54, &written, nullptr);
			WriteFile(out, data.data(), (DWORD)data.size(), &written, nullptr);
			CloseHandle(out);
		};
		writeOne(path + ".bmp", pixels);
		writeOne(path + "_alpha.bmp", alpha);
		s_Context->Unmap(staging, 0);
		s_DumpResult += " " + path.substr(path.find_last_of('\\') + 1);
	}
	staging->Release();
}

void Depth_RequestDump(const std::string& folder)
{
	s_DumpFolder = folder;
	s_DumpRequested = true;
	s_DumpResult = "dump requested";
}

static bool s_Diagnostics = false;

void Depth_SetDiagnostics(bool enabled)
{
	s_Diagnostics = enabled;
}

static void ProbeUi()
{
	if (!s_Diagnostics) { return; }
	if (!s_UiBefore || !s_UiAfter || ++s_UiProbeTick % 60 != 0) { return; }
	if (s_UiDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && s_UiDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && s_UiDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && s_UiDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
	{
		s_UiProbeText = "format " + std::to_string((int)s_UiDesc.Format) + " not probed";
		return;
	}
	for (int i = 0; i < 2; i++)
	{
		if (s_UiProbe[i]) { continue; }
		D3D11_TEXTURE2D_DESC desc = s_UiDesc;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.MiscFlags = 0;
		if (FAILED(s_Device->CreateTexture2D(&desc, nullptr, &s_UiProbe[i]))) { s_UiProbe[i] = nullptr; return; }
	}
	s_InCopy = true;
	o_CopyResource(s_Context, s_UiProbe[0], s_UiBefore);
	o_CopyResource(s_Context, s_UiProbe[1], s_UiAfter);
	s_InCopy = false;
	D3D11_MAPPED_SUBRESOURCE a{}, b{};
	if (FAILED(s_Context->Map(s_UiProbe[0], 0, D3D11_MAP_READ, 0, &a))) { return; }
	if (FAILED(s_Context->Map(s_UiProbe[1], 0, D3D11_MAP_READ, 0, &b))) { s_Context->Unmap(s_UiProbe[0], 0); return; }
	double alphaBefore = 0, alphaAfter = 0, rgbBefore = 0, rgbAfter = 0;
	int changed = 0, opaqueAfter = 0, count = 0;
	for (int gy = 0; gy < 36; gy++)
	{
		for (int gx = 0; gx < 64; gx++)
		{
			unsigned x = (unsigned)((gx + 0.5) * s_UiDesc.Width / 64);
			unsigned y = (unsigned)((gy + 0.5) * s_UiDesc.Height / 36);
			const unsigned char* pa = (const unsigned char*)a.pData + y * a.RowPitch + x * 4;
			const unsigned char* pb = (const unsigned char*)b.pData + y * b.RowPitch + x * 4;
			alphaBefore += pa[3] / 255.0;
			alphaAfter += pb[3] / 255.0;
			rgbBefore += (pa[0] + pa[1] + pa[2]) / 765.0;
			rgbAfter += (pb[0] + pb[1] + pb[2]) / 765.0;
			int d = std::max(std::abs(pa[0] - pb[0]), std::max(std::abs(pa[1] - pb[1]), std::abs(pa[2] - pb[2])));
			if (d > 5) { changed++; }
			if (pb[3] > 25) { opaqueAfter++; }
			count++;
		}
	}
	s_Context->Unmap(s_UiProbe[1], 0);
	s_Context->Unmap(s_UiProbe[0], 0);
	char text[256];
	snprintf(text, sizeof(text), "fmt %d | alpha before %.2f after %.2f | brightness before %.2f after %.2f | changed %d%% | alpha>0.1 after %d%%",
		(int)s_UiDesc.Format, alphaBefore / count, alphaAfter / count, rgbBefore / count, rgbAfter / count, changed * 100 / count, opaqueAfter * 100 / count);
	s_UiProbeText = text;
}

static void ReleaseUiCopies()
{
	if (s_UiBeforeSrv) { s_UiBeforeSrv->Release(); s_UiBeforeSrv = nullptr; }
	if (s_UiAfterSrv) { s_UiAfterSrv->Release(); s_UiAfterSrv = nullptr; }
	if (s_UiBefore) { s_UiBefore->Release(); s_UiBefore = nullptr; }
	for (auto& probe : s_UiProbe) { if (probe) { probe->Release(); probe = nullptr; } }
	if (s_UiAfter) { s_UiAfter->Release(); s_UiAfter = nullptr; }
	if (s_UiHeld) { s_UiHeld->Release(); s_UiHeld = nullptr; }
	s_UiReady = false;
	s_UiDesc = D3D11_TEXTURE2D_DESC{};
}

static bool EnsureUiCopies(ID3D11Texture2D* source)
{
	D3D11_TEXTURE2D_DESC src;
	source->GetDesc(&src);
	if (src.SampleDesc.Count > 1 || src.ArraySize != 1) { return false; }
	if (s_UiBefore && s_UiDesc.Width == src.Width && s_UiDesc.Height == src.Height && s_UiDesc.Format == src.Format) { return true; }
	ReleaseUiCopies();
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = src.Width;
	desc.Height = src.Height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = src.Format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(s_Device->CreateTexture2D(&desc, nullptr, &s_UiBefore)) || FAILED(s_Device->CreateTexture2D(&desc, nullptr, &s_UiAfter)))
	{
		ReleaseUiCopies();
		return false;
	}
	if (FAILED(s_Device->CreateShaderResourceView(s_UiBefore, nullptr, &s_UiBeforeSrv)) || FAILED(s_Device->CreateShaderResourceView(s_UiAfter, nullptr, &s_UiAfterSrv)))
	{
		ReleaseUiCopies();
		return false;
	}
	s_UiDesc = src;
	return true;
}

static void DetectUiPass(const std::vector<DepthPass>& passes)
{
	int screenPass = -1;
	for (int i = (int)passes.size() - 1; i >= 1; i--)
	{
		if (passes[i].backBuffer && passes[i].targets > 0 && passes[i].draws >= 1 && passes[i].draws <= 4) { screenPass = i; break; }
	}
	s_UiPassIndex = -1;
	s_UiTexture = nullptr;
	if (screenPass < 0) { return; }
	unsigned bw = passes[screenPass].width;
	unsigned bh = passes[screenPass].height;
	for (int i = screenPass - 1; i >= 0; i--)
	{
		const DepthPass& p = passes[i];
		if (p.backBuffer || p.targets <= 0) { continue; }
		if (p.width != bw || p.height != bh) { continue; }
		if (p.draws >= 8 && p.smallDraws * 10 >= p.draws * 7)
		{
			s_UiPassIndex = i;
			s_UiTexture = p.texture;
		}
		break;
	}
}

static RtInfo RtOf(ID3D11RenderTargetView* rtv)
{
	if (!rtv) { return RtInfo{}; }
	auto it = s_RtCache.find(rtv);
	if (it != s_RtCache.end()) { return it->second; }
	RtInfo info;
	ID3D11Resource* res = nullptr;
	rtv->GetResource(&res);
	if (res)
	{
		ID3D11Texture2D* tex = nullptr;
		if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
		{
			D3D11_TEXTURE2D_DESC desc;
			tex->GetDesc(&desc);
			info.texture = tex;
			info.width = desc.Width;
			info.height = desc.Height;
			info.format = desc.Format;
			tex->Release();
		}
		res->Release();
	}
	s_RtCache[rtv] = info;
	return info;
}

static void LogBind(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
	if (ctx != s_Context || s_InCopy) { return; }
	if (s_PassLog.size() >= 600) { return; }
	DepthPass pass{};
	RtInfo rt = (n > 0 && rtvs) ? RtOf(rtvs[0]) : RtInfo{};
	pass.width = rt.width;
	pass.height = rt.height;
	pass.format = rt.format;
	pass.backBuffer = rt.texture && rt.texture == s_BackBufferPtr;
	pass.depth = dsv != nullptr;
	pass.targets = (int)n;
	pass.minCount = 0xFFFFFFFFu;
	pass.texture = rt.texture;
	s_PassLog.push_back(pass);

	s_UiArmed = false;
	if (s_UiEnabled && !s_UiHeld && s_UiTexture && rt.texture == s_UiTexture && (int)s_PassLog.size() - 1 == s_UiPassIndex && n > 0 && rtvs && rtvs[0])
	{
		ID3D11Resource* res = nullptr;
		rtvs[0]->GetResource(&res);
		ID3D11Texture2D* tex = nullptr;
		if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
		{
			if (EnsureUiCopies(tex))
			{
				s_UiHeld = tex;
				s_UiArmed = true;
				s_UiArmDraws = 0;
				s_UiCaptured = false;
				tex = nullptr;
			}
			if (tex) { tex->Release(); }
		}
		if (res) { res->Release(); }
	}
}

static void LogTransfer(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src, int kind)
{
	if (ctx != s_Context || s_InCopy || !dst || (void*)dst != s_BackBufferPtr) { return; }
	if (s_PassLog.size() >= 600) { return; }
	DepthPass pass{};
	ID3D11Texture2D* tex = nullptr;
	if (src && SUCCEEDED(src->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
	{
		D3D11_TEXTURE2D_DESC desc;
		tex->GetDesc(&desc);
		pass.width = desc.Width;
		pass.height = desc.Height;
		pass.format = desc.Format;
		tex->Release();
	}
	pass.backBuffer = true;
	pass.targets = kind;
	pass.minCount = 0xFFFFFFFFu;
	s_PassLog.push_back(pass);
}

static void LogDraw(ID3D11DeviceContext* ctx, UINT count)
{
	if (ctx != s_Context || s_InCopy || s_PassLog.empty()) { return; }
	if (s_UiArmed && s_UiHeld)
	{
		if (s_UiArmDraws == 1)
		{
			s_InCopy = true;
			o_CopyResource(ctx, s_UiBefore, s_UiHeld);
			s_InCopy = false;
			s_UiCaptured = true;
			s_UiArmed = false;
		}
		s_UiArmDraws++;
	}
	DepthPass& pass = s_PassLog.back();
	pass.draws++;
	pass.minCount = std::min(pass.minCount, (unsigned)count);
	pass.maxCount = std::max(pass.maxCount, (unsigned)count);
	if (count <= 6) { pass.smallDraws++; }
}
static int   s_OtherDraws = 0;
static int   s_CommandLists = 0;
static int   s_Clears = 0;
static DepthStats s_LastStats{};
static int   s_CopiesThisSecond = 0;
static int   s_CopiesLastSecond = 0;
static int   s_FramesSinceCopy = 0;
static DWORD s_SecondStart = 0;

static DXGI_FORMAT TypelessOf(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS:
		return DXGI_FORMAT_R24G8_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_TYPELESS:
		return DXGI_FORMAT_R32_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
		return DXGI_FORMAT_R32G8X24_TYPELESS;
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_TYPELESS:
		return DXGI_FORMAT_R16_TYPELESS;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

static DXGI_FORMAT SrvOf(DXGI_FORMAT typeless)
{
	switch (typeless)
	{
	case DXGI_FORMAT_R24G8_TYPELESS:    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_R32_TYPELESS:      return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	case DXGI_FORMAT_R16_TYPELESS:      return DXGI_FORMAT_R16_UNORM;
	default:                            return DXGI_FORMAT_UNKNOWN;
	}
}

static UINT BytesPerPixel(DXGI_FORMAT typeless)
{
	switch (typeless)
	{
	case DXGI_FORMAT_R32G8X24_TYPELESS: return 8;
	case DXGI_FORMAT_R16_TYPELESS:      return 2;
	default:                            return 4;
	}
}

static float DecodeDepth(const uint8_t* p, DXGI_FORMAT typeless)
{
	switch (typeless)
	{
	case DXGI_FORMAT_R24G8_TYPELESS:
	{
		uint32_t v;
		memcpy(&v, p, 4);
		return (float)(v & 0xFFFFFF) / 16777215.0f;
	}
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	{
		float v;
		memcpy(&v, p, 4);
		return v;
	}
	case DXGI_FORMAT_R16_TYPELESS:
	{
		uint16_t v;
		memcpy(&v, p, 2);
		return (float)v / 65535.0f;
	}
	default:
		return -1.0f;
	}
}

static void ReleaseCopy()
{
	if (s_CopySrv) { s_CopySrv->Release(); s_CopySrv = nullptr; }
	if (s_Copy) { s_Copy->Release(); s_Copy = nullptr; }
	for (int i = 0; i < RingSize; i++)
	{
		if (s_Staging[i]) { s_Staging[i]->Release(); s_Staging[i] = nullptr; }
		s_Pending[i].clear();
		s_Used[i] = false;
	}
	s_HasCopy = false;
	memset(&s_CopyDesc, 0, sizeof(s_CopyDesc));
}

static bool EnsureCopy(const D3D11_TEXTURE2D_DESC& src)
{
	DXGI_FORMAT typeless = TypelessOf(src.Format);
	if (typeless == DXGI_FORMAT_UNKNOWN || src.SampleDesc.Count > 1) { return false; }
	if (s_Copy && s_CopyDesc.Width == src.Width && s_CopyDesc.Height == src.Height && s_CopyDesc.Format == typeless) { return true; }

	ReleaseCopy();

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = src.Width;
	desc.Height = src.Height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = typeless;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(s_Device->CreateTexture2D(&desc, nullptr, &s_Copy))) { s_Copy = nullptr; return false; }

	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = SrvOf(typeless);
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	if (FAILED(s_Device->CreateShaderResourceView(s_Copy, &srv, &s_CopySrv))) { s_CopySrv = nullptr; }

	D3D11_TEXTURE2D_DESC staging{};
	staging.Width = MaxSamples;
	staging.Height = 1;
	staging.MipLevels = 1;
	staging.ArraySize = 1;
	staging.Format = typeless;
	staging.SampleDesc.Count = 1;
	staging.Usage = D3D11_USAGE_STAGING;
	staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (int i = 0; i < RingSize; i++)
	{
		if (FAILED(s_Device->CreateTexture2D(&staging, nullptr, &s_Staging[i]))) { s_Staging[i] = nullptr; }
	}
	s_StagingFormat = typeless;
	s_CopyDesc = desc;
	return true;
}

static ID3D11Texture2D* TextureOf(ID3D11DepthStencilView* dsv)
{
	if (!dsv) { return nullptr; }
	auto it = s_Views.find(dsv);
	if (it != s_Views.end())
	{
		it->second.usedThisFrame = true;
		return it->second.texture;
	}

	ViewInfo info;
	ID3D11Resource* res = nullptr;
	dsv->GetResource(&res);
	if (res)
	{
		ID3D11Texture2D* tex = nullptr;
		if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
		{
			info.texture = tex;
			auto t = s_Textures.find(tex);
			if (t == s_Textures.end())
			{
				TexStats st;
				tex->GetDesc(&st.desc);
				s_Textures[tex] = st;
			}
			else
			{
				tex->Release();
			}
		}
		res->Release();
	}
	dsv->AddRef();
	info.usedThisFrame = true;
	s_Views[dsv] = info;
	return info.texture;
}

static TexStats* StatsOf(ID3D11Texture2D* tex)
{
	if (!tex) { return nullptr; }
	auto it = s_Textures.find(tex);
	return it == s_Textures.end() ? nullptr : &it->second;
}

static ContextSlot* SlotOf(ID3D11DeviceContext* ctx)
{
	for (auto& slot : s_Slots)
	{
		if (slot.ctx == ctx) { return &slot; }
	}
	for (auto& slot : s_Slots)
	{
		if (!slot.ctx) { slot.ctx = ctx; slot.current = nullptr; return &slot; }
	}
	return nullptr;
}

static void CopyFrom(ID3D11Texture2D* tex)
{
	TexStats* st = StatsOf(tex);
	if (!st) { return; }
	if (!EnsureCopy(st->desc)) { return; }
	s_InCopy = true;
	s_Context->CopyResource(s_Copy, tex);
	s_InCopy = false;
	s_HasCopy = true;
	s_CopiesThisSecond++;
	s_FramesSinceCopy = 0;
}

static void SetCurrent(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv)
{
	if (s_InCopy) { return; }
	ContextSlot* slot = SlotOf(ctx);
	if (!slot) { return; }
	ID3D11Texture2D* tex = TextureOf(dsv);
	TexStats* st = StatsOf(tex);
	slot->current = st;
	if (st) { st->viewsThisFrame++; }
}

static void WatchSlotsFast();

static void STDMETHODCALLTYPE Hook_OMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
	WatchSlotsFast();
	SetCurrent(ctx, dsv);
	LogBind(ctx, n, rtvs, dsv);
	o_OMSetRenderTargets(ctx, n, rtvs, dsv);
}

static void STDMETHODCALLTYPE Hook_OMSetRTsAndUAVs(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv, UINT a, UINT b, ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
{
	WatchSlotsFast();
	if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) { SetCurrent(ctx, dsv); LogBind(ctx, n, rtvs, dsv); }
	o_OMSetRTsAndUAVs(ctx, n, rtvs, dsv, a, b, uavs, counts);
}

static void STDMETHODCALLTYPE Hook_ClearDepthStencilView(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
{
	WatchSlotsFast();
	if (dsv && (flags & D3D11_CLEAR_DEPTH))
	{
		s_Clears++;
		ID3D11Texture2D* tex = TextureOf(dsv);
		TexStats* st = StatsOf(tex);
		if (st)
		{
			if (ctx == s_Context && tex == s_Chosen && st->drawsSinceClear > 20) { CopyFrom(tex); }
			st->drawsSinceClear = 0;
		}
	}
	o_ClearDepthStencilView(ctx, dsv, flags, depth, stencil);
}

static inline void CountDraw(ID3D11DeviceContext* ctx)
{
	if (s_InCopy) { return; }
	if (ctx == s_Context) { s_ImmediateDraws++; }
	else { s_OtherDraws++; }
	ContextSlot* slot = SlotOf(ctx);
	if (slot && slot->current)
	{
		slot->current->drawsThisFrame++;
		slot->current->drawsSinceClear++;
	}
}

static void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* ctx, UINT a, UINT b, INT c)
{
	CountDraw(ctx);
	LogDraw(ctx, a);
	o_DrawIndexed(ctx, a, b, c);
}

static void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* ctx, UINT a, UINT b)
{
	CountDraw(ctx);
	LogDraw(ctx, a);
	o_Draw(ctx, a, b);
}

static void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT a, UINT b, UINT c, INT d, UINT e)
{
	CountDraw(ctx);
	LogDraw(ctx, a);
	o_DrawIndexedInstanced(ctx, a, b, c, d, e);
}

static void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* ctx, UINT a, UINT b, UINT c, UINT d)
{
	CountDraw(ctx);
	LogDraw(ctx, a);
	o_DrawInstanced(ctx, a, b, c, d);
}

static void STDMETHODCALLTYPE Hook_CopyResource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src)
{
	LogTransfer(ctx, dst, src, -1);
	o_CopyResource(ctx, dst, src);
}

static void STDMETHODCALLTYPE Hook_ResolveSubresource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT a, ID3D11Resource* src, UINT b, DXGI_FORMAT f)
{
	LogTransfer(ctx, dst, src, -2);
	o_ResolveSubresource(ctx, dst, a, src, b, f);
}

static void STDMETHODCALLTYPE Hook_ExecuteCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore)
{
	if (ctx == s_Context) { s_CommandLists++; }
	o_ExecuteCommandList(ctx, list, restore);
}

static void* Swap(int index, void* hook)
{
	void* original = s_Vtable[index];
	DWORD old = 0;
	VirtualProtect(&s_Vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
	s_Vtable[index] = hook;
	VirtualProtect(&s_Vtable[index], sizeof(void*), old, &old);
	return original;
}

struct WatchedSlot
{
	const char* name;
	int         index;
	void*       hook;
	void**      original;
	void*       real;
	int         restored;
	std::string owner;
};

static WatchedSlot s_Watched[] = {
	{ "DrawIndexed", 12, nullptr, nullptr, nullptr, 0, "" },
	{ "Draw", 13, nullptr, nullptr, nullptr, 0, "" },
	{ "DrawIndexedInstanced", 20, nullptr, nullptr, nullptr, 0, "" },
	{ "DrawInstanced", 21, nullptr, nullptr, nullptr, 0, "" },
	{ "OMSetRenderTargets", 33, nullptr, nullptr, nullptr, 0, "" },
	{ "ExecuteCommandList", 58, nullptr, nullptr, nullptr, 0, "" },
};
static int s_WatchTick = 0;

static std::string OwnerOf(void* address)
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module) || !module) { return "unknown"; }
	char path[MAX_PATH];
	DWORD len = GetModuleFileNameA(module, path, MAX_PATH);
	if (len == 0) { return "unknown"; }
	std::string full(path, len);
	size_t slash = full.find_last_of("\\/");
	return slash == std::string::npos ? full : full.substr(slash + 1);
}

static void WatchSlots()
{
	if (!s_Vtable) { return; }
	bool refreshNames = (++s_WatchTick % 120) == 1;
	for (auto& w : s_Watched)
	{
		if (!w.hook) { continue; }
		void* current = s_Vtable[w.index];
		if (current != w.hook && current == w.real)
		{
			DWORD old = 0;
			VirtualProtect(&s_Vtable[w.index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
			s_Vtable[w.index] = w.hook;
			VirtualProtect(&s_Vtable[w.index], sizeof(void*), old, &old);
			w.restored++;
			current = w.hook;
		}
		if (refreshNames) { w.owner = current == w.hook ? std::string("ours") : OwnerOf(current); }
	}
}

static void WatchSlotsFast()
{
	if (!s_Vtable) { return; }
	for (int i = 0; i < 4; i++)
	{
		WatchedSlot& w = s_Watched[i];
		if (!w.hook) { continue; }
		void* current = s_Vtable[w.index];
		if (current == w.hook || current != w.real) { continue; }
		DWORD old = 0;
		VirtualProtect(&s_Vtable[w.index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
		s_Vtable[w.index] = w.hook;
		VirtualProtect(&s_Vtable[w.index], sizeof(void*), old, &old);
		w.restored++;
	}
}

std::string Depth_HookReport()
{
	std::string out;
	for (const auto& w : s_Watched)
	{
		if (!out.empty()) { out += " | "; }
		out += std::string(w.name) + ": " + (w.owner.empty() ? "?" : w.owner);
		if (w.restored > 0) { out += " (re-hooked " + std::to_string(w.restored) + ")"; }
	}
	return out;
}

static void Restore(int index, void* original, void* hook)
{
	if (!original || s_Vtable[index] != hook) { return; }
	DWORD old = 0;
	VirtualProtect(&s_Vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
	s_Vtable[index] = original;
	VirtualProtect(&s_Vtable[index], sizeof(void*), old, &old);
}

typedef HRESULT (WINAPI *PFN_GridCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static const char* GridVSSource = R"(
float4 main(uint id : SV_VertexID) : SV_POSITION
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

static const char* GridPSSource = R"(
Texture2D<float> depthTex : register(t0);
cbuffer GridInfo : register(b0)
{
	float4 info;
};
float4 main(float4 pos : SV_POSITION) : SV_Target
{
	float2 cell = floor(pos.xy);
	float2 cellSize = info.xy / info.zw;
	float d[25];
	float lo = 1.0;
	float hi = 0.0;
	float sum = 0.0;
	float sx = 0.0;
	float sy = 0.0;
	[unroll] for (int j = 0; j < 5; j++)
	{
		[unroll] for (int i = 0; i < 5; i++)
		{
			float2 p = (cell + (float2(i, j) + 0.5) / 5.0) * cellSize;
			int2 ip = int2(clamp(p, float2(0, 0), info.xy - 1.0));
			float v = depthTex.Load(int3(ip, 0));
			d[j * 5 + i] = v;
			lo = min(lo, v);
			hi = max(hi, v);
			sum += v;
			sx += v * (i - 2);
			sy += v * (j - 2);
		}
	}
	float a = sum / 25.0;
	float b = sx / 50.0;
	float c = sy / 50.0;
	float res = 0.0;
	[unroll] for (int j2 = 0; j2 < 5; j2++)
	{
		[unroll] for (int i2 = 0; i2 < 5; i2++)
		{
			res = max(res, abs(d[j2 * 5 + i2] - (a + b * (i2 - 2) + c * (j2 - 2))));
		}
	}
	return float4(lo, hi, a, res);
}
)";

static const int GridW = 192;
static const int GridH = 108;
static const int GridRing = 3;

static ID3D11VertexShader*     s_GridVS = nullptr;
static ID3D11PixelShader*      s_GridPS = nullptr;
static ID3D11Buffer*           s_GridCB = nullptr;
static ID3D11Texture2D*        s_GridTex = nullptr;
static ID3D11RenderTargetView* s_GridRTV = nullptr;
static ID3D11Texture2D*        s_GridStaging[GridRing] = {};
static int                     s_GridStagingBatch[GridRing] = {};
static bool                    s_GridStagingUsed[GridRing] = {};
static int                     s_GridRingIndex = 0;
static std::vector<float>      s_GridData;
static int                     s_GridBatch = -1;
static bool                    s_GridFailed = false;
static std::string             s_GridStatus = "not started";

static ID3DBlob* CompileGrid(const char* source, const char* profile)
{
	static PFN_GridCompile compile = nullptr;
	if (!compile)
	{
		HMODULE lib = LoadLibraryA("d3dcompiler_47.dll");
		if (lib) { compile = (PFN_GridCompile)GetProcAddress(lib, "D3DCompile"); }
	}
	if (!compile) { s_GridStatus = "D3DCompile not available"; return nullptr; }
	ID3DBlob* code = nullptr;
	ID3DBlob* errors = nullptr;
	HRESULT hr = compile(source, strlen(source), "LyricHudGrid", nullptr, nullptr, "main", profile, 0, 0, &code, &errors);
	if (FAILED(hr) || !code)
	{
		s_GridStatus = std::string("grid shader failed: ") + (errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "");
		if (errors) { errors->Release(); }
		return nullptr;
	}
	if (errors) { errors->Release(); }
	return code;
}

static void ReleaseGrid()
{
	if (s_GridRTV) { s_GridRTV->Release(); s_GridRTV = nullptr; }
	if (s_GridTex) { s_GridTex->Release(); s_GridTex = nullptr; }
	for (int i = 0; i < GridRing; i++)
	{
		if (s_GridStaging[i]) { s_GridStaging[i]->Release(); s_GridStaging[i] = nullptr; }
		s_GridStagingUsed[i] = false;
	}
	if (s_GridCB) { s_GridCB->Release(); s_GridCB = nullptr; }
	if (s_GridPS) { s_GridPS->Release(); s_GridPS = nullptr; }
	if (s_GridVS) { s_GridVS->Release(); s_GridVS = nullptr; }
	s_GridData.clear();
	s_GridBatch = -1;
	s_GridFailed = false;
	s_GridStatus = "not started";
}

static bool EnsureGrid()
{
	if (s_GridRTV && s_GridVS && s_GridPS && s_GridCB) { return true; }
	if (s_GridFailed || !s_Device) { return false; }

	ID3DBlob* vs = CompileGrid(GridVSSource, "vs_4_0");
	ID3DBlob* ps = vs ? CompileGrid(GridPSSource, "ps_4_0") : nullptr;
	bool ok = vs && ps;
	if (ok) { ok = SUCCEEDED(s_Device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s_GridVS)); }
	if (ok) { ok = SUCCEEDED(s_Device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s_GridPS)); }
	if (vs) { vs->Release(); }
	if (ps) { ps->Release(); }

	if (ok)
	{
		D3D11_BUFFER_DESC cb{};
		cb.ByteWidth = 16;
		cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		ok = SUCCEEDED(s_Device->CreateBuffer(&cb, nullptr, &s_GridCB));
	}
	if (ok)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = GridW;
		desc.Height = GridH;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		ok = SUCCEEDED(s_Device->CreateTexture2D(&desc, nullptr, &s_GridTex));
		if (ok) { ok = SUCCEEDED(s_Device->CreateRenderTargetView(s_GridTex, nullptr, &s_GridRTV)); }
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (int i = 0; ok && i < GridRing; i++)
		{
			ok = SUCCEEDED(s_Device->CreateTexture2D(&desc, nullptr, &s_GridStaging[i]));
		}
	}
	if (!ok)
	{
		std::string status = s_GridStatus;
		ReleaseGrid();
		s_GridFailed = true;
		s_GridStatus = status == "not started" ? "grid resources failed" : status;
		return false;
	}
	s_GridStatus = "ready";
	return true;
}

static void RunGridPass(int batch)
{
	if (!s_HasCopy || !s_CopySrv || !EnsureGrid()) { return; }
	int slot = s_GridRingIndex;
	if (s_GridStagingUsed[slot]) { return; }

	ID3D11DeviceContext* ctx = s_Context;

	ID3D11RenderTargetView* oldRTV[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* oldDSV = nullptr;
	ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRTV, &oldDSV);
	UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT oldVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	ctx->RSGetViewports(&vpCount, oldVP);
	ID3D11RasterizerState* oldRS = nullptr;
	ctx->RSGetState(&oldRS);
	ID3D11BlendState* oldBS = nullptr;
	float oldBF[4];
	UINT oldSM = 0;
	ctx->OMGetBlendState(&oldBS, oldBF, &oldSM);
	ID3D11DepthStencilState* oldDSS = nullptr;
	UINT oldRef = 0;
	ctx->OMGetDepthStencilState(&oldDSS, &oldRef);
	ID3D11ShaderResourceView* oldSRV = nullptr;
	ctx->PSGetShaderResources(0, 1, &oldSRV);
	ID3D11Buffer* oldCB = nullptr;
	ctx->PSGetConstantBuffers(0, 1, &oldCB);
	ID3D11PixelShader* oldPS = nullptr;
	ID3D11ClassInstance* psInst[256];
	UINT psInstCount = 256;
	ctx->PSGetShader(&oldPS, psInst, &psInstCount);
	ID3D11VertexShader* oldVS = nullptr;
	ID3D11ClassInstance* vsInst[256];
	UINT vsInstCount = 256;
	ctx->VSGetShader(&oldVS, vsInst, &vsInstCount);
	ID3D11GeometryShader* oldGS = nullptr;
	ID3D11ClassInstance* gsInst[256];
	UINT gsInstCount = 256;
	ctx->GSGetShader(&oldGS, gsInst, &gsInstCount);
	ID3D11HullShader* oldHS = nullptr;
	ID3D11ClassInstance* hsInst[256];
	UINT hsInstCount = 256;
	ctx->HSGetShader(&oldHS, hsInst, &hsInstCount);
	ID3D11DomainShader* oldDS = nullptr;
	ID3D11ClassInstance* dsInst[256];
	UINT dsInstCount = 256;
	ctx->DSGetShader(&oldDS, dsInst, &dsInstCount);
	D3D11_PRIMITIVE_TOPOLOGY oldTopo;
	ctx->IAGetPrimitiveTopology(&oldTopo);
	ID3D11InputLayout* oldIL = nullptr;
	ctx->IAGetInputLayout(&oldIL);

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(ctx->Map(s_GridCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		float info[4] = { (float)s_CopyDesc.Width, (float)s_CopyDesc.Height, (float)GridW, (float)GridH };
		memcpy(mapped.pData, info, sizeof(info));
		ctx->Unmap(s_GridCB, 0);
	}

	s_InCopy = true;
	ctx->OMSetRenderTargets(1, &s_GridRTV, nullptr);
	D3D11_VIEWPORT vp{};
	vp.Width = (float)GridW;
	vp.Height = (float)GridH;
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);
	ctx->RSSetState(nullptr);
	ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	ctx->OMSetDepthStencilState(nullptr, 0);
	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(s_GridVS, nullptr, 0);
	ctx->GSSetShader(nullptr, nullptr, 0);
	ctx->HSSetShader(nullptr, nullptr, 0);
	ctx->DSSetShader(nullptr, nullptr, 0);
	ctx->PSSetShader(s_GridPS, nullptr, 0);
	ctx->PSSetShaderResources(0, 1, &s_CopySrv);
	ctx->PSSetConstantBuffers(0, 1, &s_GridCB);
	ctx->Draw(3, 0);

	ID3D11ShaderResourceView* noSRV = nullptr;
	ctx->PSSetShaderResources(0, 1, &noSRV);
	ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRTV, oldDSV);
	ctx->RSSetViewports(vpCount, oldVP);
	ctx->RSSetState(oldRS);
	ctx->OMSetBlendState(oldBS, oldBF, oldSM);
	ctx->OMSetDepthStencilState(oldDSS, oldRef);
	ctx->PSSetShaderResources(0, 1, &oldSRV);
	ctx->PSSetConstantBuffers(0, 1, &oldCB);
	ctx->PSSetShader(oldPS, psInst, psInstCount);
	ctx->VSSetShader(oldVS, vsInst, vsInstCount);
	ctx->GSSetShader(oldGS, gsInst, gsInstCount);
	ctx->HSSetShader(oldHS, hsInst, hsInstCount);
	ctx->DSSetShader(oldDS, dsInst, dsInstCount);
	ctx->IASetPrimitiveTopology(oldTopo);
	ctx->IASetInputLayout(oldIL);
	s_InCopy = false;

	for (auto* v : oldRTV) { if (v) { v->Release(); } }
	if (oldDSV) { oldDSV->Release(); }
	if (oldRS) { oldRS->Release(); }
	if (oldBS) { oldBS->Release(); }
	if (oldDSS) { oldDSS->Release(); }
	if (oldSRV) { oldSRV->Release(); }
	if (oldCB) { oldCB->Release(); }
	if (oldPS) { oldPS->Release(); }
	for (UINT i = 0; i < psInstCount; i++) { if (psInst[i]) { psInst[i]->Release(); } }
	if (oldVS) { oldVS->Release(); }
	for (UINT i = 0; i < vsInstCount; i++) { if (vsInst[i]) { vsInst[i]->Release(); } }
	if (oldGS) { oldGS->Release(); }
	for (UINT i = 0; i < gsInstCount; i++) { if (gsInst[i]) { gsInst[i]->Release(); } }
	if (oldHS) { oldHS->Release(); }
	for (UINT i = 0; i < hsInstCount; i++) { if (hsInst[i]) { hsInst[i]->Release(); } }
	if (oldDS) { oldDS->Release(); }
	for (UINT i = 0; i < dsInstCount; i++) { if (dsInst[i]) { dsInst[i]->Release(); } }
	if (oldIL) { oldIL->Release(); }

	ctx->CopyResource(s_GridStaging[slot], s_GridTex);
	s_GridStagingBatch[slot] = batch;
	s_GridStagingUsed[slot] = true;
	s_GridRingIndex = (s_GridRingIndex + 1) % GridRing;
}

static void ReadGrid()
{
	for (int k = 0; k < GridRing; k++)
	{
		int slot = (s_GridRingIndex + k) % GridRing;
		if (!s_GridStagingUsed[slot] || !s_GridStaging[slot]) { continue; }
		D3D11_MAPPED_SUBRESOURCE mapped{};
		HRESULT hr = s_Context->Map(s_GridStaging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { continue; }
		if (FAILED(hr)) { s_GridStagingUsed[slot] = false; continue; }
		s_GridData.resize(GridW * GridH * 4);
		for (int y = 0; y < GridH; y++)
		{
			memcpy(&s_GridData[y * GridW * 4], (const uint8_t*)mapped.pData + y * mapped.RowPitch, GridW * 4 * sizeof(float));
		}
		s_Context->Unmap(s_GridStaging[slot], 0);
		s_GridStagingUsed[slot] = false;
		s_GridBatch = s_GridStagingBatch[slot];
		s_GridStatus = "ready";
	}
}

bool Depth_GetGrid(const float*& data, int& width, int& height, int& batch)
{
	if (s_GridData.empty() || s_GridBatch < 0) { return false; }
	data = s_GridData.data();
	width = GridW;
	height = GridH;
	batch = s_GridBatch;
	return true;
}

std::string Depth_GridStatus()
{
	return s_GridStatus;
}

static void ReleaseTracking()
{
	for (auto& kv : s_Views) { kv.first->Release(); }
	s_Views.clear();
	for (auto& kv : s_Textures) { kv.first->Release(); }
	s_Textures.clear();
	for (auto& slot : s_Slots) { slot.ctx = nullptr; slot.current = nullptr; }
	s_Chosen = nullptr;
}

bool Depth_Install(void* swapChain)
{
	if (s_Installed) { return true; }
	if (!swapChain) { s_Status = "no swap chain"; return false; }

	IDXGISwapChain* sc = (IDXGISwapChain*)swapChain;
	s_SwapChain = sc;
	if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&s_Device)) || !s_Device)
	{
		s_Status = "no D3D11 device";
		return false;
	}
	s_Device->GetImmediateContext(&s_Context);
	if (!s_Context)
	{
		s_Device->Release();
		s_Device = nullptr;
		s_Status = "no device context";
		return false;
	}

	s_Vtable = *(void***)s_Context;
	o_DrawIndexed           = (PFN_DrawIndexed)Swap(VT_DrawIndexed, (void*)&Hook_DrawIndexed);
	o_Draw                  = (PFN_Draw)Swap(VT_Draw, (void*)&Hook_Draw);
	o_DrawIndexedInstanced  = (PFN_DrawIndexedInstanced)Swap(VT_DrawIndexedInstanced, (void*)&Hook_DrawIndexedInstanced);
	o_DrawInstanced         = (PFN_DrawInstanced)Swap(VT_DrawInstanced, (void*)&Hook_DrawInstanced);
	o_OMSetRenderTargets    = (PFN_OMSetRenderTargets)Swap(VT_OMSetRenderTargets, (void*)&Hook_OMSetRenderTargets);
	o_OMSetRTsAndUAVs       = (PFN_OMSetRenderTargetsAndUAVs)Swap(VT_OMSetRTsAndUAVs, (void*)&Hook_OMSetRTsAndUAVs);
	o_ClearDepthStencilView = (PFN_ClearDepthStencilView)Swap(VT_ClearDepthStencil, (void*)&Hook_ClearDepthStencilView);
	o_ExecuteCommandList    = (PFN_ExecuteCommandList)Swap(VT_ExecuteCommandList, (void*)&Hook_ExecuteCommandList);
	o_CopyResource          = (PFN_CopyResource)Swap(47, (void*)&Hook_CopyResource);
	o_ResolveSubresource    = (PFN_ResolveSubresource)Swap(57, (void*)&Hook_ResolveSubresource);

	void* hooks[6] = { (void*)&Hook_DrawIndexed, (void*)&Hook_Draw, (void*)&Hook_DrawIndexedInstanced, (void*)&Hook_DrawInstanced, (void*)&Hook_OMSetRenderTargets, (void*)&Hook_ExecuteCommandList };
	void** originals[6] = { (void**)&o_DrawIndexed, (void**)&o_Draw, (void**)&o_DrawIndexedInstanced, (void**)&o_DrawInstanced, (void**)&o_OMSetRenderTargets, (void**)&o_ExecuteCommandList };
	for (int i = 0; i < 6; i++)
	{
		s_Watched[i].hook = hooks[i];
		s_Watched[i].original = originals[i];
		s_Watched[i].real = *originals[i];
		s_Watched[i].restored = 0;
		s_Watched[i].owner = OwnerOf(*originals[i]) + " before us";
	}

	s_SecondStart = GetTickCount();
	s_Installed = true;
	s_Status = "installed, waiting for frames";
	return true;
}

void Depth_Uninstall()
{
	if (!s_Installed) { return; }
	Restore(VT_DrawIndexed, (void*)o_DrawIndexed, (void*)&Hook_DrawIndexed);
	Restore(VT_Draw, (void*)o_Draw, (void*)&Hook_Draw);
	Restore(VT_DrawIndexedInstanced, (void*)o_DrawIndexedInstanced, (void*)&Hook_DrawIndexedInstanced);
	Restore(VT_DrawInstanced, (void*)o_DrawInstanced, (void*)&Hook_DrawInstanced);
	Restore(VT_OMSetRenderTargets, (void*)o_OMSetRenderTargets, (void*)&Hook_OMSetRenderTargets);
	Restore(VT_OMSetRTsAndUAVs, (void*)o_OMSetRTsAndUAVs, (void*)&Hook_OMSetRTsAndUAVs);
	Restore(VT_ClearDepthStencil, (void*)o_ClearDepthStencilView, (void*)&Hook_ClearDepthStencilView);
	Restore(VT_ExecuteCommandList, (void*)o_ExecuteCommandList, (void*)&Hook_ExecuteCommandList);
	Restore(47, (void*)o_CopyResource, (void*)&Hook_CopyResource);
	Restore(57, (void*)o_ResolveSubresource, (void*)&Hook_ResolveSubresource);

	ReleaseGrid();
	ReleaseCopy();
	ReleaseUiCopies();
	s_UiPassIndex = -1;
	s_UiTexture = nullptr;
	ReleaseTracking();
	s_Results.clear();
	if (s_Context) { s_Context->Release(); s_Context = nullptr; }
	if (s_Device) { s_Device->Release(); s_Device = nullptr; }
	s_Installed = false;
	s_Status = "not installed";
}

bool Depth_IsInstalled()
{
	return s_Installed;
}

void Depth_ResetTracking()
{
	if (!s_Installed) { return; }
	ReleaseTracking();
	s_Challenger = nullptr;
	s_ChallengeFrames = 0;
	for (int i = 0; i < RingSize; i++) { s_Pending[i].clear(); s_Used[i] = false; }
	s_Results.clear();
	s_HasCopy = false;
	s_GridData.clear();
	s_GridBatch = -1;
	for (int i = 0; i < GridRing; i++) { s_GridStagingUsed[i] = false; }
}

static int Score(const TexStats& st);

static int Score(const TexStats& st)
{
	if (st.desc.SampleDesc.Count > 1) { return -1; }
	if (TypelessOf(st.desc.Format) == DXGI_FORMAT_UNKNOWN) { return -1; }
	float wr = st.desc.Width / s_ScreenW;
	float hr = st.desc.Height / s_ScreenH;
	if (wr < 0.45f || hr < 0.45f || wr > 4.5f || hr > 4.5f) { return -1; }
	float aspectDiff = std::fabs((float)st.desc.Width / st.desc.Height - s_ScreenW / s_ScreenH);
	if (aspectDiff > 0.1f) { return -1; }
	if (st.viewsLastFrame == 0 && st.drawsLastFrame == 0) { return -1; }
	int score = st.drawsLastFrame * 100 + st.viewsLastFrame * 10;
	if (st.desc.Format == DXGI_FORMAT_R24G8_TYPELESS || st.desc.Format == DXGI_FORMAT_R32_TYPELESS || st.desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS) { score += 5; }
	return score;
}

static void ReadBack()
{
	for (int k = 0; k < RingSize; k++)
	{
		int slot = (s_RingIndex + k) % RingSize;
		if (!s_Used[slot] || !s_Staging[slot]) { continue; }
		D3D11_MAPPED_SUBRESOURCE mapped{};
		HRESULT hr = s_Context->Map(s_Staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { continue; }
		if (FAILED(hr)) { s_Used[slot] = false; continue; }

		std::vector<DepthSample> out = s_Pending[slot];
		UINT bpp = BytesPerPixel(s_StagingFormat);
		for (size_t i = 0; i < out.size() && i < (size_t)MaxSamples; i++)
		{
			if (!out[i].valid) { continue; }
			out[i].raw = DecodeDepth((const uint8_t*)mapped.pData + i * bpp, s_StagingFormat);
		}
		s_Context->Unmap(s_Staging[slot], 0);
		s_Used[slot] = false;
		s_Results = out;
	}
}

void Depth_EndFrame(float screenWidth, float screenHeight, int batch)
{
	if (!s_Installed) { return; }
	s_ScreenW = std::max(1.0f, screenWidth);
	s_ScreenH = std::max(1.0f, screenHeight);

	if (s_Chosen) { CopyFrom(s_Chosen); }
	s_FramesSinceCopy++;

	WatchSlots();
	s_UiArmed = false;
	if (s_UiHeld && !s_UiCaptured)
	{
		s_UiHeld->Release();
		s_UiHeld = nullptr;
		if (++s_UiMisses > 3) { s_UiReady = false; }
	}
	if (s_UiHeld)
	{
		s_InCopy = true;
		o_CopyResource(s_Context, s_UiAfter, s_UiHeld);
		s_InCopy = false;
		s_UiHeld->Release();
		s_UiHeld = nullptr;
		s_UiCaptured = false;
		s_UiReady = true;
		s_UiMisses = 0;
		ProbeUi();
		if (s_DumpRequested && !s_DumpFolder.empty())
		{
			s_DumpRequested = false;
			s_DumpResult = "wrote";
			WriteBmp(s_DumpFolder + "\\dump_before", s_UiBefore);
			WriteBmp(s_DumpFolder + "\\dump_after", s_UiAfter);
			if (s_SwapChain)
			{
				ID3D11Texture2D* back = nullptr;
				if (SUCCEEDED(s_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back)
				{
					WriteBmp(s_DumpFolder + "\\dump_screen", back);
					back->Release();
				}
			}
		}
	}
	else if (++s_UiMisses > 3)
	{
		s_UiReady = false;
	}
	s_LastPasses.swap(s_PassLog);
	DetectUiPass(s_LastPasses);
	s_PassLog.clear();
	if (++s_PassFrames >= 600) { s_PassFrames = 0; s_RtCache.clear(); }
	if (s_SwapChain)
	{
		ID3D11Texture2D* back = nullptr;
		if (SUCCEEDED(s_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back)
		{
			if (back != s_BackBufferPtr) { s_RtCache.clear(); }
			s_BackBufferPtr = back;
			back->Release();
		}
	}

	ID3D11Texture2D* best = nullptr;
	int bestScore = 0;
	int chosenScore = -1;
	int plausible = 0;
	for (auto& kv : s_Textures)
	{
		TexStats& st = kv.second;
		st.drawsLastFrame = st.drawsThisFrame;
		st.viewsLastFrame = st.viewsThisFrame;
		st.drawsThisFrame = 0;
		st.viewsThisFrame = 0;
		st.idleFrames = st.viewsLastFrame > 0 ? 0 : st.idleFrames + 1;
		int score = Score(st);
		if (score > 0) { plausible++; }
		if (kv.first == s_Chosen) { chosenScore = score; }
		if (score > bestScore)
		{
			bestScore = score;
			best = kv.first;
		}
	}
	if (!s_Chosen)
	{
		if (best) { s_Chosen = best; s_Switches++; }
		s_Challenger = nullptr;
		s_ChallengeFrames = 0;
	}
	else if (best && best != s_Chosen && bestScore > std::max(0, chosenScore))
	{
		if (best != s_Challenger)
		{
			s_Challenger = best;
			s_ChallengeFrames = 0;
		}
		if (++s_ChallengeFrames >= 60)
		{
			s_Chosen = best;
			s_Challenger = nullptr;
			s_ChallengeFrames = 0;
			s_Switches++;
		}
	}
	else
	{
		s_Challenger = nullptr;
		s_ChallengeFrames = 0;
	}

	for (auto it = s_Views.begin(); it != s_Views.end();)
	{
		it->second.idleFrames = it->second.usedThisFrame ? 0 : it->second.idleFrames + 1;
		it->second.usedThisFrame = false;
		if (it->second.idleFrames > 300)
		{
			for (auto& slot : s_Slots) { slot.current = nullptr; }
			it->first->Release();
			it = s_Views.erase(it);
		}
		else
		{
			++it;
		}
	}
	for (auto it = s_Textures.begin(); it != s_Textures.end();)
	{
		bool referenced = false;
		for (auto& v : s_Views) { if (v.second.texture == it->first) { referenced = true; break; } }
		if (!referenced && it->first != s_Chosen)
		{
			for (auto& slot : s_Slots) { slot.current = nullptr; }
			if (it->first == s_Challenger) { s_Challenger = nullptr; s_ChallengeFrames = 0; }
			it->first->Release();
			it = s_Textures.erase(it);
		}
		else
		{
			++it;
		}
	}

	s_LastStats.immediateDraws = s_ImmediateDraws;
	s_LastStats.otherDraws = s_OtherDraws;
	s_LastStats.commandLists = s_CommandLists;
	s_LastStats.clears = s_Clears;
	s_ImmediateDraws = 0;
	s_OtherDraws = 0;
	s_CommandLists = 0;
	s_Clears = 0;

	DWORD now = GetTickCount();
	if (now - s_SecondStart >= 1000)
	{
		s_CopiesLastSecond = s_CopiesThisSecond;
		s_CopiesThisSecond = 0;
		s_SecondStart = now;
	}

	ReadBack();
	RunGridPass(batch);
	ReadGrid();

	if (!s_Chosen) { s_Status = "no scene depth buffer found yet"; }
	else if (!s_HasCopy) { s_Status = "depth buffer found, no copy yet"; }
	else { s_Status = "capturing"; }
}

void Depth_RequestSamples(const std::vector<DepthSample>& samples)
{
	if (!s_Installed || !s_HasCopy || !s_Copy) { return; }
	int slot = s_RingIndex;
	if (s_Used[slot] || !s_Staging[slot]) { return; }

	std::vector<DepthSample> pending = samples;
	if (pending.size() > (size_t)MaxSamples) { pending.resize(MaxSamples); }

	float sx = s_CopyDesc.Width / s_ScreenW;
	float sy = s_CopyDesc.Height / s_ScreenH;
	for (size_t i = 0; i < pending.size(); i++)
	{
		int px = (int)(pending[i].screenX * sx);
		int py = (int)(pending[i].screenY * sy);
		pending[i].raw = -1.0f;
		pending[i].valid = pending[i].screenX >= 0.0f && px >= 0 && py >= 0 && px < (int)s_CopyDesc.Width && py < (int)s_CopyDesc.Height;
		if (!pending[i].valid) { continue; }
		D3D11_BOX box{ (UINT)px, (UINT)py, 0, (UINT)px + 1, (UINT)py + 1, 1 };
		s_Context->CopySubresourceRegion(s_Staging[slot], 0, (UINT)i, 0, 0, s_Copy, 0, &box);
	}
	s_Pending[slot] = pending;
	s_Used[slot] = true;
	s_RingIndex = (s_RingIndex + 1) % RingSize;
}

std::vector<DepthSample> Depth_GetSamples()
{
	return s_Results;
}

std::vector<DepthCandidate> Depth_GetCandidates()
{
	std::vector<DepthCandidate> out;
	for (const auto& kv : s_Textures)
	{
		if (kv.second.drawsLastFrame == 0 && kv.second.viewsLastFrame == 0) { continue; }
		DepthCandidate c;
		c.width = kv.second.desc.Width;
		c.height = kv.second.desc.Height;
		c.format = (unsigned)kv.second.desc.Format;
		c.samples = kv.second.desc.SampleDesc.Count;
		c.draws = kv.second.drawsLastFrame;
		c.views = kv.second.viewsLastFrame;
		c.chosen = kv.first == s_Chosen;
		out.push_back(c);
	}
	std::sort(out.begin(), out.end(), [](const DepthCandidate& a, const DepthCandidate& b) { return a.draws > b.draws; });
	if (out.size() > 8) { out.resize(8); }
	return out;
}

void Depth_SetUiMaskEnabled(bool enabled)
{
	s_UiEnabled = enabled;
	if (!enabled) { s_UiReady = false; }
}

bool Depth_GetUiMask(void*& before, void*& after)
{
	before = s_UiBeforeSrv;
	after = s_UiAfterSrv;
	return s_UiEnabled && s_UiReady && s_UiBeforeSrv && s_UiAfterSrv;
}

std::string Depth_UiStatus()
{
	if (!s_UiEnabled) { return "off"; }
	if (s_UiPassIndex < 0) { return "UI step not found"; }
	return "UI step #" + std::to_string(s_UiPassIndex) + (s_UiReady ? ", hiding active" : ", waiting") + " | " + s_UiProbeText + " | " + s_DumpResult;
}

std::vector<DepthPass> Depth_GetPasses()
{
	return s_LastPasses;
}

DepthStats Depth_GetStats()
{
	DepthStats st = s_LastStats;
	st.copiesLastSecond = s_CopiesLastSecond;
	st.framesSinceCopy = s_FramesSinceCopy;
	st.trackedViews = (int)s_Views.size();
	st.trackedTextures = (int)s_Textures.size();
	st.switches = s_Switches;
	return st;
}

void* Depth_GetPreview(unsigned& width, unsigned& height)
{
	width = s_CopyDesc.Width;
	height = s_CopyDesc.Height;
	return s_HasCopy ? (void*)s_CopySrv : nullptr;
}

void* Depth_GetDevice()
{
	return s_Device;
}

void* Depth_GetContext()
{
	return s_Context;
}

std::string Depth_GetStatus()
{
	return s_Status;
}

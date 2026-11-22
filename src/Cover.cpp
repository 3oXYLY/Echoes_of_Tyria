#include <windows.h>
#include <d3d11.h>

#include "Cover.h"

static ID3D11ShaderResourceView* s_View = nullptr;
static int s_Version = -1;

void Cover_Release()
{
	if (s_View) { s_View->Release(); }
	s_View = nullptr;
	s_Version = -1;
}

void* Cover_View()
{
	return s_View;
}

int Cover_Version()
{
	return s_Version;
}

bool Cover_Update(void* swapChain, int version, const unsigned char* pixels, int width, int height)
{
	if (version == s_Version) { return s_View != nullptr; }
	Cover_Release();
	s_Version = version;
	if (!swapChain || !pixels || width <= 0 || height <= 0) { return false; }

	ID3D11Device* device = nullptr;
	if (FAILED(((IDXGISwapChain*)swapChain)->GetDevice(__uuidof(ID3D11Device), (void**)&device)) || !device) { return false; }

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = (UINT)width;
	desc.Height = (UINT)height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA init{};
	init.pSysMem = pixels;
	init.SysMemPitch = (UINT)width * 4;

	ID3D11Texture2D* texture = nullptr;
	if (SUCCEEDED(device->CreateTexture2D(&desc, &init, &texture)) && texture)
	{
		device->CreateShaderResourceView(texture, nullptr, &s_View);
		texture->Release();
	}
	device->Release();
	return s_View != nullptr;
}

#pragma once

bool  Cover_Update(void* swapChain, int version, const unsigned char* pixels, int width, int height);
void* Cover_View();
int   Cover_Version();
void  Cover_Release();

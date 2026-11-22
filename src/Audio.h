#pragma once

#include <string>

void  Audio_Start();
void  Audio_Stop();
void  Audio_SetSource(const std::string& appId);
float Audio_Pulse();
bool  Audio_Active();
std::string Audio_Status();

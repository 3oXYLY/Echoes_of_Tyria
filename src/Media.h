#pragma once

#include <memory>
#include <string>
#include <vector>

struct MediaInfo
{
	bool        available = false;
	bool        playing   = false;
	std::string artist;
	std::string title;
	std::string appId;
	double      position  = 0.0;
	double      duration  = 0.0;
	std::string error;
	bool        hasAccent = false;
	float       accent[3] = { 1.0f, 1.0f, 1.0f };
	float       accent2[3] = { 1.0f, 0.82f, 0.3f };
	int         coverVersion = 0;
	std::shared_ptr<const std::vector<unsigned char>> cover;
	int         coverSize = 0;
	double      syncDrift = 0.0;
	int         syncUpdates = 0;
};

enum class MediaCommand { None = 0, PlayPause, Next, Previous };

void Media_Command(MediaCommand command);
void Media_Start();
void Media_Stop();
MediaInfo Media_Get();

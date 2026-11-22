#pragma once

#include <memory>
#include <string>
#include <vector>

struct LyricLine
{
	double      time;
	std::string text;
};

enum class LyricsStatus
{
	Idle,
	Loading,
	Found,
	NotFound,
	Error
};

struct LyricsSnapshot
{
	LyricsStatus status = LyricsStatus::Idle;
	std::string  trackKey;
	std::string  message;
	std::shared_ptr<const std::vector<LyricLine>> lines;
};

std::string MakeTrackKey(const std::string& artist, const std::string& title);

void Lyrics_SetFolder(const std::string& folder);
void Lyrics_Start();
void Lyrics_Stop();
LyricsSnapshot Lyrics_Get();

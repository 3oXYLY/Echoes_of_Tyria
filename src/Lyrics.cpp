#include "Lyrics.h"
#include "Media.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

#include "json.hpp"

static std::mutex        s_Mutex;
static LyricsSnapshot    s_Snapshot;
static std::atomic<bool> s_Running{ false };
static std::thread       s_Thread;
static std::map<std::string, std::shared_ptr<const std::vector<LyricLine>>> s_Cache;
static std::mutex        s_FolderMutex;
static std::string       s_Folder;

std::string MakeTrackKey(const std::string& artist, const std::string& title)
{
	return artist + "\x1f" + title;
}

static void SetSnapshot(const LyricsSnapshot& snap)
{
	std::lock_guard<std::mutex> lock(s_Mutex);
	s_Snapshot = snap;
}

static void SleepWhileRunning(int milliseconds)
{
	for (int waited = 0; waited < milliseconds && s_Running; waited += 25)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
}

static std::string UrlEncode(const std::string& s)
{
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s)
	{
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
		{
			out += (char)c;
		}
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

static bool HttpGet(const std::string& pathAndQuery, std::string& body, int& status)
{
	body.clear();
	status = 0;

	HINTERNET session = WinHttpOpen(L"EchoesOfTyria/1.0 (Guild Wars 2 Nexus addon)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
	{
		session = WinHttpOpen(L"EchoesOfTyria/1.0 (Guild Wars 2 Nexus addon)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	}
	if (!session) { return false; }
	WinHttpSetTimeouts(session, 3000, 3000, 4000, 6000);

	bool ok = false;
	HINTERNET connect = WinHttpConnect(session, L"lrclib.net", INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (connect)
	{
		std::wstring wpath(pathAndQuery.begin(), pathAndQuery.end());
		HINTERNET request = WinHttpOpenRequest(connect, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
		if (request)
		{
			if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr))
			{
				DWORD code = 0;
				DWORD size = sizeof(code);
				WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
				status = (int)code;

				ok = true;
				for (;;)
				{
					DWORD available = 0;
					if (!WinHttpQueryDataAvailable(request, &available)) { ok = false; break; }
					if (available == 0) { break; }
					std::string chunk(available, '\0');
					DWORD read = 0;
					if (!WinHttpReadData(request, &chunk[0], available, &read)) { ok = false; break; }
					chunk.resize(read);
					body += chunk;
					if (!s_Running) { ok = false; break; }
				}
			}
			WinHttpCloseHandle(request);
		}
		WinHttpCloseHandle(connect);
	}
	WinHttpCloseHandle(session);
	return ok;
}

static void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos)
	{
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
}

static std::string CleanText(std::string s)
{
	ReplaceAll(s, "\xE2\x80\x99", "'");
	ReplaceAll(s, "\xE2\x80\x98", "'");
	ReplaceAll(s, "\xE2\x80\x9C", "\"");
	ReplaceAll(s, "\xE2\x80\x9D", "\"");
	ReplaceAll(s, "\xE2\x80\xA6", "...");
	ReplaceAll(s, "\xE2\x80\x93", "-");
	ReplaceAll(s, "\xE2\x80\x94", "-");
	ReplaceAll(s, "\r", "");
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) { s.pop_back(); }
	size_t start = 0;
	while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) { start++; }
	return s.substr(start);
}

static std::vector<LyricLine> ParseLrc(const std::string& lrc)
{
	std::vector<LyricLine> lines;
	size_t pos = 0;
	while (pos <= lrc.size())
	{
		size_t end = lrc.find('\n', pos);
		if (end == std::string::npos) { end = lrc.size(); }
		std::string row = lrc.substr(pos, end - pos);
		pos = end + 1;

		std::vector<double> times;
		size_t i = 0;
		while (i < row.size() && row[i] == '[')
		{
			size_t close = row.find(']', i);
			if (close == std::string::npos) { break; }
			int minutes = 0;
			double seconds = 0.0;
			std::string tag = row.substr(i + 1, close - i - 1);
			if (sscanf(tag.c_str(), "%d:%lf", &minutes, &seconds) == 2)
			{
				times.push_back(minutes * 60.0 + seconds);
			}
			i = close + 1;
		}
		if (times.empty()) { continue; }

		std::string text = CleanText(row.substr(i));
		for (double t : times) { lines.push_back({ t, text }); }
	}
	std::stable_sort(lines.begin(), lines.end(), [](const LyricLine& a, const LyricLine& b) { return a.time < b.time; });
	return lines;
}

static std::string SimplifyTitle(const std::string& title)
{
	std::string t = title;
	size_t dash = t.find(" - ");
	if (dash != std::string::npos) { t = t.substr(0, dash); }
	size_t paren = t.find(" (feat");
	if (paren == std::string::npos) { paren = t.find(" (with"); }
	if (paren != std::string::npos) { t = t.substr(0, paren); }
	return t;
}

static bool IsAltSpeed(const std::string& title)
{
	std::string t;
	for (char c : title) { t += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
	const char* marks[] = { "slowed", "sped up", "speed up", "sped-up", "spedup", "nightcore", "daycore", "speed" };
	for (const char* m : marks) { if (t.find(m) != std::string::npos) { return true; } }
	return false;
}

static bool SearchSynced(const std::string& query, double duration, bool allowStretch, std::string& outLrc, double& outItemDuration, std::string& outError)
{
	std::string body;
	int status = 0;
	if (!HttpGet(query, body, status))
	{
		outError = "network error";
		return false;
	}
	if (status != 200)
	{
		outError = "server returned " + std::to_string(status);
		return false;
	}

	nlohmann::json data = nlohmann::json::parse(body, nullptr, false);
	if (data.is_discarded() || !data.is_array())
	{
		outError = "bad response";
		return false;
	}

	double bestDiff = 1e9;
	bool found = false;
	for (const auto& item : data)
	{
		if (!item.is_object()) { continue; }
		auto it = item.find("syncedLyrics");
		if (it == item.end() || !it->is_string()) { continue; }
		std::string lrc = it->get<std::string>();
		if (lrc.empty()) { continue; }

		double itemDuration = 0.0;
		auto d = item.find("duration");
		if (d != item.end() && d->is_number()) { itemDuration = d->get<double>(); }
		double diff = duration > 0.0 ? std::fabs(itemDuration - duration) : 0.0;
		if (!found || diff < bestDiff)
		{
			bestDiff = diff;
			outLrc = lrc;
			outItemDuration = itemDuration;
			found = true;
		}
	}
	if (found && duration > 0.0 && bestDiff > 15.0)
	{
		double ratio = outItemDuration > 0.0 ? duration / outItemDuration : 0.0;
		found = allowStretch && ratio > 0.6 && ratio < 1.7;
	}
	return found;
}

static std::string Normalize(const std::string& s)
{
	std::string out;
	for (unsigned char c : s)
	{
		if (c >= 'A' && c <= 'Z') { out += (char)(c - 'A' + 'a'); }
		else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) { out += (char)c; }
	}
	return out;
}

static std::string ToUtf8(const wchar_t* w)
{
	int size = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (size <= 1) { return std::string(); }
	std::string out(size - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], size, nullptr, nullptr);
	return out;
}

static std::wstring ToWide(const std::string& s)
{
	int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	if (size <= 1) { return std::wstring(); }
	std::wstring out(size - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], size);
	return out;
}

static bool FindLocalLyrics(const std::string& artist, const std::string& title, std::string& outLrc)
{
	std::string folder;
	{
		std::lock_guard<std::mutex> lock(s_FolderMutex);
		folder = s_Folder;
	}
	if (folder.empty()) { return false; }

	std::string simple = SimplifyTitle(title);
	std::vector<std::string> wanted = {
		Normalize(artist + " - " + title),
		Normalize(artist + " - " + simple),
		Normalize(title),
		Normalize(simple)
	};

	std::wstring dir = ToWide(folder);
	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileW((dir + L"\\*.lrc").c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) { return false; }
	std::wstring bestPath;
	size_t bestRank = wanted.size();
	do
	{
		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { continue; }
		std::string name = ToUtf8(data.cFileName);
		size_t dot = name.find_last_of('.');
		std::string stem = Normalize(dot == std::string::npos ? name : name.substr(0, dot));
		for (size_t r = 0; r < wanted.size() && r < bestRank; r++)
		{
			if (!wanted[r].empty() && stem == wanted[r])
			{
				bestRank = r;
				bestPath = dir + L"\\" + data.cFileName;
				break;
			}
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
	if (bestPath.empty()) { return false; }

	HANDLE file = CreateFileW(bestPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) { return false; }
	std::string content;
	char buffer[4096];
	DWORD read = 0;
	while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) { content.append(buffer, read); }
	CloseHandle(file);
	if (content.size() >= 3 && (unsigned char)content[0] == 0xEF && (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF) { content.erase(0, 3); }
	outLrc = content;
	return !ParseLrc(content).empty();
}

static std::string s_LocalContent;

static bool LoadLocal(const std::string& key, const std::string& artist, const std::string& title)
{
	std::string lrc;
	if (!FindLocalLyrics(artist, title, lrc)) { s_LocalContent.clear(); return false; }
	if (lrc == s_LocalContent && Lyrics_Get().trackKey == key) { return true; }
	s_LocalContent = lrc;
	auto lines = std::make_shared<const std::vector<LyricLine>>(ParseLrc(lrc));
	LyricsSnapshot snap;
	snap.trackKey = key;
	snap.status = LyricsStatus::Found;
	snap.lines = lines;
	snap.message = std::to_string(lines->size()) + " lines, local file";
	SetSnapshot(snap);
	return true;
}

static void FetchLyrics(const std::string& key, const std::string& artist, const std::string& title, double duration)
{
	std::string lrc;
	std::string error;
	std::string simple = SimplifyTitle(title);

	std::vector<std::string> queries;
	queries.push_back("/api/search?track_name=" + UrlEncode(title) + "&artist_name=" + UrlEncode(artist));
	if (simple != title)
	{
		queries.push_back("/api/search?track_name=" + UrlEncode(simple) + "&artist_name=" + UrlEncode(artist));
	}
	queries.push_back("/api/search?q=" + UrlEncode(artist + " " + simple));

	bool found = false;
	bool networkProblem = false;
	bool altSpeed = IsAltSpeed(title);
	double itemDuration = 0.0;
	for (const auto& q : queries)
	{
		if (!s_Running) { return; }
		std::string attemptError;
		if (SearchSynced(q, duration, altSpeed, lrc, itemDuration, attemptError))
		{
			found = true;
			break;
		}
		if (!attemptError.empty())
		{
			networkProblem = true;
			error = attemptError;
		}
	}

	LyricsSnapshot snap;
	snap.trackKey = key;
	if (found)
	{
		std::vector<LyricLine> parsed = ParseLrc(lrc);
		double ratio = 1.0;
		if (altSpeed && duration > 0.0 && itemDuration > 0.0 && std::fabs(duration - itemDuration) > 1.5)
		{
			ratio = duration / itemDuration;
			for (auto& line : parsed) { line.time *= ratio; }
		}
		auto lines = std::make_shared<const std::vector<LyricLine>>(std::move(parsed));
		s_Cache[key] = lines;
		snap.status = LyricsStatus::Found;
		snap.lines = lines;
		snap.message = std::to_string(lines->size()) + " lines";
		if (ratio != 1.0)
		{
			char buffer[48];
			snprintf(buffer, sizeof(buffer), ", timing x%.2f", ratio);
			snap.message += buffer;
		}
	}
	else if (networkProblem)
	{
		snap.status = LyricsStatus::Error;
		snap.message = error;
	}
	else
	{
		s_Cache[key] = nullptr;
		snap.status = LyricsStatus::NotFound;
		snap.message = "no synced lyrics";
	}
	SetSnapshot(snap);
}

static void LyricsLoop()
{
	std::string currentKey;
	auto lastAttempt = std::chrono::steady_clock::now() - std::chrono::hours(1);
	auto lastLocalCheck = lastAttempt;
	bool lastWasError = false;
	bool usingLocal = false;
	std::string waitingKey;
	auto waitingSince = std::chrono::steady_clock::now();

	while (s_Running)
	{
		MediaInfo media = Media_Get();
		if (media.available && !media.title.empty())
		{
			std::string key = MakeTrackKey(media.artist, media.title);
			if (key != currentKey && media.duration <= 0.0)
			{
				if (key != waitingKey) { waitingKey = key; waitingSince = std::chrono::steady_clock::now(); }
				if (std::chrono::steady_clock::now() - waitingSince < std::chrono::seconds(3)) { SleepWhileRunning(100); continue; }
			}
			bool retry = lastWasError && key == currentKey && std::chrono::steady_clock::now() - lastAttempt > std::chrono::seconds(15);
			if (key == currentKey && !usingLocal && std::chrono::steady_clock::now() - lastLocalCheck > std::chrono::seconds(3))
			{
				lastLocalCheck = std::chrono::steady_clock::now();
				if (LoadLocal(key, media.artist, media.title)) { usingLocal = true; lastWasError = false; }
			}
			else if (key == currentKey && usingLocal && std::chrono::steady_clock::now() - lastLocalCheck > std::chrono::seconds(3))
			{
				lastLocalCheck = std::chrono::steady_clock::now();
				if (!LoadLocal(key, media.artist, media.title)) { usingLocal = false; currentKey.clear(); }
			}
			if (key != currentKey || retry)
			{
				currentKey = key;
				lastLocalCheck = std::chrono::steady_clock::now();
				s_LocalContent.clear();
				usingLocal = LoadLocal(key, media.artist, media.title);
				if (usingLocal) { lastWasError = false; SleepWhileRunning(250); continue; }
				auto cached = s_Cache.find(key);
				if (cached != s_Cache.end())
				{
					LyricsSnapshot snap;
					snap.trackKey = key;
					snap.status = cached->second ? LyricsStatus::Found : LyricsStatus::NotFound;
					snap.lines = cached->second;
					snap.message = cached->second ? std::to_string(cached->second->size()) + " lines (cached)" : "no synced lyrics";
					SetSnapshot(snap);
					lastWasError = false;
				}
				else
				{
					LyricsSnapshot loading;
					loading.trackKey = key;
					loading.status = LyricsStatus::Loading;
					loading.message = "loading";
					SetSnapshot(loading);

					lastAttempt = std::chrono::steady_clock::now();
					FetchLyrics(key, media.artist, media.title, media.duration);
					lastWasError = Lyrics_Get().status == LyricsStatus::Error;
				}
			}
		}
		SleepWhileRunning(250);
	}
}

void Lyrics_SetFolder(const std::string& folder)
{
	std::lock_guard<std::mutex> lock(s_FolderMutex);
	s_Folder = folder;
}

void Lyrics_Start()
{
	if (s_Running) { return; }
	s_Running = true;
	s_Thread = std::thread(LyricsLoop);
}

void Lyrics_Stop()
{
	s_Running = false;
	if (s_Thread.joinable()) { s_Thread.join(); }
}

LyricsSnapshot Lyrics_Get()
{
	std::lock_guard<std::mutex> lock(s_Mutex);
	return s_Snapshot;
}

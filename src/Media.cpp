#include "Media.h"

#if defined(_MSC_VER)

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <mutex>
#include <thread>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Graphics.Imaging.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Graphics::Imaging;

struct MediaState
{
	bool         available = false;
	bool         playing   = false;
	std::string  artist;
	std::string  title;
	std::string  appId;
	double       rawPosition = 0.0;
	double       duration    = 0.0;
	winrt::clock::time_point lastUpdated{};
	std::string  error;
	bool         hasAccent = false;
	float        accent[3] = { 1.0f, 1.0f, 1.0f };
	float        accent2[3] = { 1.0f, 0.82f, 0.3f };
	int          coverVersion = 0;
	std::shared_ptr<const std::vector<unsigned char>> cover;
	int          coverSize = 0;
	double       syncDrift = 0.0;
	int          syncUpdates = 0;
};

static std::mutex        s_Mutex;
static MediaState        s_State;
static std::atomic<bool> s_Running{ false };
static std::thread       s_Thread;
static std::atomic<int>  s_Command{ 0 };

static void SleepWhileRunning(int milliseconds)
{
	for (int waited = 0; waited < milliseconds && s_Running; waited += 25)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
}

static float HueOf(float r, float g, float b)
{
	float hi = std::max(r, std::max(g, b));
	float lo = std::min(r, std::min(g, b));
	float d = hi - lo;
	if (d <= 1e-5f) { return -1.0f; }
	float h;
	if (hi == r) { h = std::fmod((g - b) / d, 6.0f); }
	else if (hi == g) { h = (b - r) / d + 2.0f; }
	else { h = (r - g) / d + 4.0f; }
	h *= 60.0f;
	return h < 0.0f ? h + 360.0f : h;
}

static void Brighten(float c[3])
{
	float hi = std::max(c[0], std::max(c[1], c[2]));
	float target = std::max(hi, 0.85f);
	for (int k = 0; k < 3; k++) { c[k] = std::min(1.0f, c[k] / std::max(hi, 0.01f) * target); }
}

static bool ReadCover(GlobalSystemMediaTransportControlsSessionMediaProperties const& props, float accent[3], float accent2[3], std::vector<unsigned char>& bytes)
{
	auto thumb = props.Thumbnail();
	if (!thumb) { return false; }
	auto stream = thumb.OpenReadAsync().get();
	uint64_t size = stream.Size();
	if (size == 0 || size > 16ull * 1024 * 1024) { return false; }

	auto decoder = BitmapDecoder::CreateAsync(stream).get();
	BitmapTransform transform;
	transform.ScaledWidth(48);
	transform.ScaledHeight(48);
	transform.InterpolationMode(BitmapInterpolationMode::Linear);
	auto provider = decoder.GetPixelDataAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore, transform, ExifOrientationMode::IgnoreExifOrientation, ColorManagementMode::DoNotColorManage).get();
	auto pixels = provider.DetachPixelData();
	size_t count = pixels.size() / 4;
	if (count == 0) { return false; }

	std::vector<std::pair<float, size_t>> scored;
	scored.reserve(count);
	for (size_t i = 0; i < count; i++)
	{
		float b = pixels[i * 4] / 255.0f;
		float g = pixels[i * 4 + 1] / 255.0f;
		float r = pixels[i * 4 + 2] / 255.0f;
		float hi = std::max(r, std::max(g, b));
		float lo = std::min(r, std::min(g, b));
		float sat = hi > 0.0f ? (hi - lo) / hi : 0.0f;
		scored.push_back({ sat * sat * hi, i });
	}
	std::sort(scored.begin(), scored.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
	size_t take = std::max<size_t>(1, count / 7);
	float sum[3] = { 0, 0, 0 };
	for (size_t k = 0; k < take; k++)
	{
		size_t i = scored[k].second;
		sum[0] += pixels[i * 4 + 2] / 255.0f;
		sum[1] += pixels[i * 4 + 1] / 255.0f;
		sum[2] += pixels[i * 4] / 255.0f;
	}
	for (int c = 0; c < 3; c++) { accent[c] = sum[c] / (float)take; }
	float hi = std::max(accent[0], std::max(accent[1], accent[2]));
	float lo = std::min(accent[0], std::min(accent[1], accent[2]));
	bool gray = hi - lo < 0.08f;
	if (gray) { accent[0] = accent[1] = accent[2] = 1.0f; }
	else { Brighten(accent); }

	float mainHue = gray ? -1.0f : HueOf(accent[0], accent[1], accent[2]);
	float sum2[3] = { 0, 0, 0 };
	size_t found = 0;
	size_t limit = std::max<size_t>(1, count / 10);
	for (size_t k = 0; k < count && found < limit; k++)
	{
		if (scored[k].first < 0.04f) { break; }
		size_t i = scored[k].second;
		float r = pixels[i * 4 + 2] / 255.0f, g = pixels[i * 4 + 1] / 255.0f, b = pixels[i * 4] / 255.0f;
		float h = HueOf(r, g, b);
		if (h < 0.0f) { continue; }
		float dh = mainHue < 0.0f ? 180.0f : std::fabs(h - mainHue);
		if (dh > 180.0f) { dh = 360.0f - dh; }
		if (dh < 55.0f) { continue; }
		sum2[0] += r; sum2[1] += g; sum2[2] += b;
		found++;
	}
	if (found >= std::max<size_t>(3, count / 60))
	{
		for (int c = 0; c < 3; c++) { accent2[c] = sum2[c] / (float)found; }
		Brighten(accent2);
	}
	else if (!gray)
	{
		float h2 = std::max(accent[0], std::max(accent[1], accent[2]));
		float l2 = std::min(accent[0], std::min(accent[1], accent[2]));
		for (int c = 0; c < 3; c++) { accent2[c] = h2 + l2 - accent[c]; }
		Brighten(accent2);
	}
	else
	{
		accent2[0] = 1.0f; accent2[1] = 0.82f; accent2[2] = 0.3f;
	}

	BitmapTransform large;
	large.ScaledWidth(256);
	large.ScaledHeight(256);
	large.InterpolationMode(BitmapInterpolationMode::Fant);
	auto image = decoder.GetPixelDataAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore, large, ExifOrientationMode::RespectExifOrientation, ColorManagementMode::DoNotColorManage).get();
	auto data = image.DetachPixelData();
	bytes.assign(data.begin(), data.end());
	for (size_t i = 3; i < bytes.size(); i += 4) { bytes[i] = 255; }
	return true;
}

static void MediaLoop()
{
	winrt::init_apartment(winrt::apartment_type::multi_threaded);

	GlobalSystemMediaTransportControlsSessionManager manager{ nullptr };
	while (s_Running && !manager)
	{
		try
		{
			manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
		}
		catch (...)
		{
			std::lock_guard<std::mutex> lock(s_Mutex);
			s_State = MediaState{};
			s_State.error = "media manager unavailable";
			SleepWhileRunning(2000);
		}
	}

	std::string coverKey;
	bool coverAccent = false;
	float coverColor[3] = { 1.0f, 1.0f, 1.0f };
	float coverColor2[3] = { 1.0f, 0.82f, 0.3f };
	int coverVersion = 0;
	std::shared_ptr<const std::vector<unsigned char>> coverBytes;
	double prevRaw = -1.0;
	winrt::clock::time_point prevUpdated{};
	bool prevPlaying = false;
	std::string prevKey;
	double drift = 0.0;
	int updates = 0;

	while (s_Running)
	{
		MediaState next{};
		try
		{
			auto session = manager.GetCurrentSession();
			int command = s_Command.exchange(0);
			if (session && command != 0)
			{
				if (command == (int)MediaCommand::PlayPause) { session.TryTogglePlayPauseAsync().get(); }
				else if (command == (int)MediaCommand::Next) { session.TrySkipNextAsync().get(); }
				else if (command == (int)MediaCommand::Previous) { session.TrySkipPreviousAsync().get(); }
			}
			if (session)
			{
				auto props = session.TryGetMediaPropertiesAsync().get();
				auto timeline = session.GetTimelineProperties();
				auto playback = session.GetPlaybackInfo();

				next.available = true;
				next.artist = winrt::to_string(props.Artist());
				next.title = winrt::to_string(props.Title());
				next.appId = winrt::to_string(session.SourceAppUserModelId());
				next.rawPosition = std::chrono::duration<double>(timeline.Position()).count();
				next.duration = std::chrono::duration<double>(timeline.EndTime() - timeline.StartTime()).count();
				next.lastUpdated = timeline.LastUpdatedTime();
				next.playing = playback.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

				std::string key = next.artist + "\x1f" + next.title;
				if (key != coverKey)
				{
					coverKey = key;
					coverAccent = false;
					coverBytes.reset();
					coverVersion++;
					try
					{
						std::vector<unsigned char> bytes;
						if (ReadCover(props, coverColor, coverColor2, bytes))
						{
							coverAccent = true;
							coverBytes = std::make_shared<const std::vector<unsigned char>>(std::move(bytes));
						}
					}
					catch (...) {}
				}
				next.hasAccent = coverAccent;
				next.accent[0] = coverColor[0];
				next.accent[1] = coverColor[1];
				next.accent[2] = coverColor[2];
				next.accent2[0] = coverColor2[0];
				next.accent2[1] = coverColor2[1];
				next.accent2[2] = coverColor2[2];
				next.coverVersion = coverVersion;
				next.cover = coverBytes;
				next.coverSize = coverBytes && coverBytes->size() == 256 * 256 * 4 ? 256 : 0;
			}
		}
		catch (winrt::hresult_error const& e)
		{
			next = MediaState{};
			next.error = winrt::to_string(e.message());
		}
		catch (...)
		{
			next = MediaState{};
			next.error = "unknown media error";
		}

		if (next.available)
		{
			std::string key = next.artist + "\x1f" + next.title;
			bool changed = next.rawPosition != prevRaw || next.lastUpdated != prevUpdated;
			if (changed && key == prevKey && prevPlaying && next.playing && prevRaw >= 0.0)
			{
				double elapsed = std::chrono::duration<double>(next.lastUpdated - prevUpdated).count();
				double predicted = prevRaw + elapsed;
				double d = next.rawPosition - predicted;
				if (std::fabs(d) < 5.0) { drift = d; updates++; }
			}
			if (key != prevKey) { drift = 0.0; updates = 0; }
			prevRaw = next.rawPosition;
			prevUpdated = next.lastUpdated;
			prevPlaying = next.playing;
			prevKey = key;
			next.syncDrift = drift;
			next.syncUpdates = updates;
		}
		{
			std::lock_guard<std::mutex> lock(s_Mutex);
			s_State = next;
		}
		SleepWhileRunning(250);
	}

	winrt::uninit_apartment();
}

void Media_Command(MediaCommand command)
{
	s_Command = (int)command;
}

void Media_Start()
{
	if (s_Running) { return; }
	s_Running = true;
	s_Thread = std::thread(MediaLoop);
}

void Media_Stop()
{
	s_Running = false;
	if (s_Thread.joinable()) { s_Thread.join(); }
}

MediaInfo Media_Get()
{
	MediaState state;
	{
		std::lock_guard<std::mutex> lock(s_Mutex);
		state = s_State;
	}

	MediaInfo info;
	info.available = state.available;
	info.playing = state.playing;
	info.artist = state.artist;
	info.title = state.title;
	info.appId = state.appId;
	info.duration = state.duration;
	info.error = state.error;
	info.hasAccent = state.hasAccent;
	info.accent[0] = state.accent[0];
	info.accent[1] = state.accent[1];
	info.accent[2] = state.accent[2];
	info.accent2[0] = state.accent2[0];
	info.accent2[1] = state.accent2[1];
	info.accent2[2] = state.accent2[2];
	info.coverVersion = state.coverVersion;
	info.cover = state.cover;
	info.coverSize = state.coverSize;
	info.syncDrift = state.syncDrift;
	info.syncUpdates = state.syncUpdates;
	info.position = state.rawPosition;
	if (state.playing && state.lastUpdated.time_since_epoch().count() != 0)
	{
		double age = std::chrono::duration<double>(winrt::clock::now() - state.lastUpdated).count();
		if (age > 0.0 && age < 3600.0) { info.position += age; }
	}
	if (info.duration > 0.0 && info.position > info.duration) { info.position = info.duration; }
	return info;
}

#else

void Media_Command(MediaCommand) {}
void Media_Start() {}
void Media_Stop() {}
MediaInfo Media_Get()
{
	MediaInfo info;
	info.error = "media needs an MSVC build";
	return info;
}

#endif

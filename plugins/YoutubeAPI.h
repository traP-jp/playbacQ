#pragma once

#include <drogon/drogon.h>
#include <cstdint>
#include <optional>
#include <string>

namespace YoutubeAPI {
	struct VideoInfo {
		std::optional<std::string> title;
		std::optional<std::string> description;
		std::optional<int32_t> duration;
		std::optional<std::string> type;
	};

	struct LiveInfo {
		std::optional<std::string> actualStartTime;
		std::optional<std::string> actualEndTime;
	};

	drogon::Task<std::optional<VideoInfo>> fetchVideoInfo(
		const std::string& videoId,
		const std::string& apiKey
	);

	drogon::Task<std::optional<LiveInfo>> fetchLiveInfo(
		const std::string& videoId,
		const std::string& apiKey
	);
}

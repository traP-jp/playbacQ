#pragma once

#include <drogon/drogon.h>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace YoutubeAPI {
	struct VideoInfo {
		std::optional<std::string> title;
		std::optional<std::string> description;
		std::optional<int32_t> duration;
		std::optional<std::string> type;
		std::optional<uint64_t> viewCount;
		std::optional<uint64_t> likeCount;
		std::optional<uint64_t> commentCount;
		Json::Value publicMetadata;
	};

	drogon::Task<std::optional<VideoInfo>> fetchVideoInfo(
		const std::string& videoId,
		const std::string& apiKey
	);

	drogon::Task<std::optional<std::unordered_map<std::string, VideoInfo>>> fetchVideoInfos(
		const std::vector<std::string>& videoIds,
		const std::string& apiKey
	);
}
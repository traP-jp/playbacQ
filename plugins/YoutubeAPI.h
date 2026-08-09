#pragma once

#include <drogon/drogon.h>
#include <drogon/HttpClient.h>
#include <json/json.h>
#include <string>
#include <optional>

namespace YoutubeAPI {
	drogon::Task<std::optional<Json::Value>> fetchVideoInfo(const std::string& videoId, const std::string& apiKey);
}
#include "YoutubeAPI.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <regex>

namespace {
	std::string youtubeApiBaseUrl() {
		const char* configuredUrl = std::getenv("YOUTUBE_API_BASE_URL");
		return configuredUrl != nullptr && configuredUrl[0] != '\0'
			? std::string(configuredUrl)
			: "https://www.googleapis.com";
	}

	std::optional<int32_t> parseDuration(const Json::Value& value) {
		if (!value.isString()) {
			return std::nullopt;
		}

		const std::string duration = value.asString();
		const std::regex iso8601Regex(
			R"(^P(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?)?$)"
		);
		std::smatch match;
		if (!std::regex_match(duration, match, iso8601Regex)) {
			return std::nullopt;
		}
		if (!match[1].matched && !match[2].matched && !match[3].matched && !match[4].matched) {
			return std::nullopt;
		}

		int64_t totalSeconds = 0;
		if (match[1].matched) totalSeconds += std::stoll(match[1].str()) * 86400;
		if (match[2].matched) totalSeconds += std::stoll(match[2].str()) * 3600;
		if (match[3].matched) totalSeconds += std::stoll(match[3].str()) * 60;
		if (match[4].matched) totalSeconds += std::stoll(match[4].str());
		if (totalSeconds > std::numeric_limits<int32_t>::max()) {
			return std::nullopt;
		}
		return static_cast<int32_t>(totalSeconds);
	}
}

namespace YoutubeAPI {
	drogon::Task<std::optional<VideoInfo>> fetchVideoInfo(
		const std::string& videoId,
		const std::string& apiKey
	) {
		if (videoId.empty()) {
			co_return std::nullopt;
		}

		auto client = drogon::HttpClient::newHttpClient(youtubeApiBaseUrl());
		auto req = drogon::HttpRequest::newHttpRequest();
		req->setMethod(drogon::Get);
		req->setPath("/youtube/v3/videos");
		req->setParameter("part", "snippet,contentDetails");
		req->setParameter("id", videoId);
		req->setParameter("key", apiKey);

		try {
			auto resp = co_await client->sendRequestCoro(req);
			if (resp->getStatusCode() != drogon::k200OK) {
				std::cerr << "Failed to fetch video info. HTTP Status: "
					<< resp->getStatusCode() << std::endl;
				co_return std::nullopt;
			}

			auto json = resp->getJsonObject();
			if (!json || !(*json)["items"].isArray() || (*json)["items"].empty()) {
				std::cerr << "Invalid videos.list response" << std::endl;
				co_return std::nullopt;
			}

			const Json::Value& item = (*json)["items"][0];
			if (!item.isObject() || !item["id"].isString() || item["id"].asString() != videoId) {
				std::cerr << "videos.list returned an unexpected video resource" << std::endl;
				co_return std::nullopt;
			}

			const Json::Value& snippet = item["snippet"];
			const Json::Value& contentDetails = item["contentDetails"];
			VideoInfo info;
			if (snippet.isObject()) {
				if (snippet["title"].isString()) {
					info.title = snippet["title"].asString();
				}
				if (snippet["description"].isString()) {
					info.description = snippet["description"].asString();
				}
				if (snippet["liveBroadcastContent"].isString()) {
					const std::string broadcastContent = snippet["liveBroadcastContent"].asString();
					if (broadcastContent == "live" || broadcastContent == "upcoming") {
						info.type = "youtube live";
					} else if (broadcastContent == "none") {
						info.type = "youtube";
					}
				}
			}
			if (contentDetails.isObject()) {
				info.duration = parseDuration(contentDetails["duration"]);
			}
			co_return info;
		}
		catch (const std::exception& e) {
			std::cerr << "Error fetching video info: " << e.what() << std::endl;
			co_return std::nullopt;
		}
	}
}

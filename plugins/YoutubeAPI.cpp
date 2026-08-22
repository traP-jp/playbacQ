#include "YoutubeAPI.h"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <regex>
#include <sstream>

namespace {
	std::string youtubeApiBaseUrl() {
		const char* configuredUrl = std::getenv("YOUTUBE_API_BASE_URL");
		return configuredUrl != nullptr && configuredUrl[0] != '\0'
			? std::string(configuredUrl)
			: "https://www.googleapis.com";
	}

	std::optional<uint64_t> parseUnsigned(const Json::Value& value) {
		if (value.isUInt64()) {
			return value.asUInt64();
		}
		if (!value.isString()) {
			return std::nullopt;
		}

		const std::string text = value.asString();
		uint64_t parsed = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
		if (error != std::errc() || end != text.data() + text.size()) {
			return std::nullopt;
		}
		return parsed;
	}

	std::optional<int32_t> parseDuration(const Json::Value& value) {
		if (!value.isString()) {
			return std::nullopt;
		}

		const std::string duration = value.asString();
		const std::regex iso8601Regex(R"(^PT(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?$)");
		std::smatch match;
		if (!std::regex_match(duration, match, iso8601Regex)) {
			return std::nullopt;
		}

		int64_t totalSeconds = 0;
		if (match[1].matched) totalSeconds += std::stoll(match[1].str()) * 3600;
		if (match[2].matched) totalSeconds += std::stoll(match[2].str()) * 60;
		if (match[3].matched) totalSeconds += std::stoll(match[3].str());
		if (totalSeconds > std::numeric_limits<int32_t>::max()) {
			return std::nullopt;
		}
		return static_cast<int32_t>(totalSeconds);
	}

	Json::Value sanitizePublicMetadata(Json::Value item) {
		// These complete parts are only available to the video owner. They are not
		// requested, and are removed defensively in case an upstream response or
		// test fixture unexpectedly contains them.
		item.removeMember("fileDetails");
		item.removeMember("processingDetails");
		item.removeMember("suggestions");

		// These individual properties live in otherwise public parts but are only
		// returned for an authorized owner/uploader request.
		if (item.isMember("contentDetails") && item["contentDetails"].isObject()) {
			item["contentDetails"].removeMember("hasCustomThumbnail");
		}
		if (item.isMember("status") && item["status"].isObject()) {
			item["status"].removeMember("selfDeclaredMadeForKids");
		}
		if (item.isMember("statistics") && item["statistics"].isObject()) {
			item["statistics"].removeMember("dislikeCount");
		}
		return item;
	}
}

namespace YoutubeAPI {
	drogon::Task<std::optional<std::unordered_map<std::string, VideoInfo>>> fetchVideoInfos(
		const std::vector<std::string>& videoIds,
		const std::string& apiKey
	) {
		if (videoIds.empty() || videoIds.size() > 50) {
			co_return std::nullopt;
		}

		std::ostringstream joinedIds;
		for (size_t i = 0; i < videoIds.size(); ++i) {
			if (i != 0) {
				joinedIds << ',';
			}
			joinedIds << videoIds[i];
		}

		auto client = drogon::HttpClient::newHttpClient(youtubeApiBaseUrl());
		auto req = drogon::HttpRequest::newHttpRequest();
		req->setMethod(drogon::Get);
		req->setPath("/youtube/v3/videos");
		// All videos.list parts that are available without video-owner
		// authorization. One videos.list call costs one quota unit for a batch of
		// up to 50 IDs, regardless of how many of these parts are requested.
		req->setParameter(
			"part",
			"id,snippet,contentDetails,status,statistics,paidProductPlacementDetails,"
			"player,topicDetails,recordingDetails,liveStreamingDetails,brandPartner,localizations"
		);
		req->setParameter("id", joinedIds.str());
		req->setParameter("key", apiKey);

		try {
			auto resp = co_await client->sendRequestCoro(req);
			if (resp->getStatusCode() != drogon::k200OK) {
				std::cerr << "Failed to fetch video info. HTTP Status: "
					<< resp->getStatusCode() << std::endl;
				co_return std::nullopt;
			}

			auto json = resp->getJsonObject();
			if (!json || !(*json)["items"].isArray()) {
				std::cerr << "Invalid videos.list response" << std::endl;
				co_return std::nullopt;
			}

			std::unordered_map<std::string, VideoInfo> infos;
			for (const auto& rawItem : (*json)["items"]) {
				if (!rawItem.isObject()) {
					std::cerr << "videos.list returned a non-object item" << std::endl;
					continue;
				}
				Json::Value item = sanitizePublicMetadata(rawItem);
				if (!item["id"].isString() || item["id"].asString().empty()) {
					std::cerr << "videos.list returned an item without an ID" << std::endl;
					continue;
				}

				const std::string videoId = item["id"].asString();
				const Json::Value& snippet = item["snippet"];
				const Json::Value& contentDetails = item["contentDetails"];
				const Json::Value& statistics = item["statistics"];

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
						info.type = broadcastContent == "live" || broadcastContent == "upcoming"
							? "youtube live"
							: "youtube";
					}
				}
				if (contentDetails.isObject()) {
					info.duration = parseDuration(contentDetails["duration"]);
				}
				if (statistics.isObject()) {
					info.viewCount = parseUnsigned(statistics["viewCount"]);
					info.likeCount = parseUnsigned(statistics["likeCount"]);
					info.commentCount = parseUnsigned(statistics["commentCount"]);
				}
				info.publicMetadata = std::move(item);
				if (!infos.emplace(videoId, std::move(info)).second) {
					std::cerr << "videos.list returned a duplicate video ID: " << videoId << std::endl;
					co_return std::nullopt;
				}
			}
			co_return infos;
		}
		catch (const std::exception& e) {
			std::cerr << "Error fetching video info: " << e.what() << std::endl;
			co_return std::nullopt;
		}
	}

	drogon::Task<std::optional<VideoInfo>> fetchVideoInfo(
		const std::string& videoId,
		const std::string& apiKey
	) {
		auto infos = co_await fetchVideoInfos({videoId}, apiKey);
		if (!infos) {
			co_return std::nullopt;
		}
		auto info = infos->find(videoId);
		if (info == infos->end()) {
			co_return std::nullopt;
		}
		co_return std::move(info->second);
	}

}
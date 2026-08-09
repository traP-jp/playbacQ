#include "YoutubeAPI.h"

namespace YoutubeAPI {
	drogon::Task<std::optional<Json::Value>> fetchVideoInfo(const std::string& videoId, const std::string& apiKey) {
		auto client = drogon::HttpClient::newHttpClient("https://www.googleapis.com");

		auto req = drogon::HttpRequest::newHttpRequest();
		req->setMethod(drogon::Get);
		req->setPath("/youtube/v3/videos");
		req->setParameter("part", "snippet,contentDetails");
		req->setParameter("id", videoId);
		req->setParameter("key", apiKey);

		try {
			auto resp = co_await client->sendRequestCoro(req);
			if (resp->getStatusCode() != drogon::k200OK) {
				std::cerr << "Failed to fetch video info. HTTP Status: " << resp->getStatusCode() << std::endl;
				co_return std::nullopt;
			}
			auto jsonPtr = resp->getJsonObject();
			if (!jsonPtr || !jsonPtr->isMember("items") || (*jsonPtr)["items"].empty()) {
				std::cerr << "No video info found for video ID: " << videoId << std::endl;
				co_return std::nullopt;
			}
			Json::Value videoInfo;
			const auto& snippet = (*jsonPtr)["items"][0]["snippet"];
			videoInfo["title"] = snippet.get("title", "").asString();
			videoInfo["description"] = snippet.get("description", "").asString();
			// 再生時間を取得する
			const std::string durationStr = (*jsonPtr)["items"][0]["contentDetails"].get("duration", "").asString();
			// ISO 8601形式の期間を秒に変換する
			std::regex iso8601Regex(R"(^PT(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?$)");
			std::smatch match;
			int totalSeconds = 0;
			if (std::regex_match(durationStr, match, iso8601Regex)) {
				if (match[1].matched) totalSeconds += std::stoi(match[1].str()) * 3600; // 時間を秒に変換
				if (match[2].matched) totalSeconds += std::stoi(match[2].str()) * 60; // 分を秒に変換
				if (match[3].matched) totalSeconds += std::stoi(match[3].str()); // 秒を加算
			}
			videoInfo["duration"] = totalSeconds;
			co_return videoInfo;
		}
		catch (const std::exception& e) {
			std::cerr << "Error fetching video info: " << e.what() << std::endl;
			co_return std::nullopt;
		}
	}
}
#pragma once

#include <drogon/plugins/Plugin.h>
#include <drogon/HttpController.h>
#include <drogon/HttpClient.h>
#include <expected>
#include <shared_mutex>
#include <chrono>
#include <atomic>

struct LiveVideo {
	// YouTubeの動画ID
	std::string videoId;
	double delay;
	std::optional<std::chrono::sys_time<std::chrono::milliseconds>> actualStartTime;
	// キーはtraQのメッセージID、値はplaybacQのコメントID
	std::map<std::string, int32_t> comments;
};

class BotPlugin : public drogon::Plugin<BotPlugin> {
private:
	// キーはチャンネルID
	std::map<std::string, LiveVideo> liveVideos;
	std::optional<std::chrono::sys_time<std::chrono::milliseconds>> iso8601ToSysTime(const std::string& iso8601);
	std::string youtubeApiKey;
	std::mutex mutex_;
	std::unordered_map<std::string, trantor::TimerId> check_timers_;
public:
	void initAndStart(const Json::Value& config) override;

	drogon::Task<void> registerChannel(
		const std::string& channelId,
		const std::string& videoId,
		double delay
	);
	drogon::Task<drogon::HttpResponsePtr> postComment(
		std::string_view channelId,
		std::string_view comment,
		std::string_view createdAt,
		std::string_view messageId
	);
	drogon::Task<drogon::HttpResponsePtr> updateComment(
		std::string_view channelId,
		std::string_view comment,
		std::string_view messageId
	);
	drogon::Task<drogon::HttpResponsePtr> deleteComment(
		std::string_view channelId,
		std::string_view messageId
	);

	void shutdown() override;
};
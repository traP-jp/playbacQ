#pragma once

#include <drogon/plugins/Plugin.h>
#include <drogon/HttpController.h>
#include <drogon/HttpClient.h>
#include <expected>
#include <shared_mutex>
#include <chrono>
#include <atomic>

struct LiveChannel {
	std::string videoId;
	std::string channelId;
};

class BotPlugin : public drogon::Plugin<BotPlugin> {
private:

public:
	void initAndStart(const Json::Value& config) override;

	void shutdown() override;
}
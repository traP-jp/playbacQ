#pragma once

#include <drogon/plugins/Plugin.h>
#include <drogon/HttpController.h>
#include <drogon/HttpClient.h>
#include <expected>
#include <shared_mutex>
#include <chrono>

struct allStampsCacheEntry {
  Json::Value stamps;
  std::chrono::steady_clock::time_point timestamp;
};

struct stampImageCacheEntry {
  std::string imageRawData;
  std::string contentType;
  std::chrono::steady_clock::time_point timestamp;
  std::chrono::steady_clock::time_point lastAccessed;
};

class traQAPI : public drogon::Plugin<traQAPI>
{
private:
  // キャッシュの有効期限は2週間とする
  constexpr static std::chrono::weeks cacheDuration{ 2 };
  // スタンプ画像のキャッシュサイズの上限(最大50MBまでキャッシュする)
  constexpr static int maxStampImageCacheSize = 50 * 1024 * 1024;

  std::string Token;
  std::string ApiUrl;
  drogon::HttpClientPtr client;
  std::shared_mutex mutex;
  allStampsCacheEntry allStampsCache;
  std::unordered_map<std::string, stampImageCacheEntry> stampImageCache;

  drogon::Task<drogon::HttpResponsePtr> getFromTraQAPI(const std::string& endpoint);
public:
  traQAPI() {}
  /// This method must be called by drogon to initialize and start the plugin.
  /// It must be implemented by the user.
  void initAndStart(const Json::Value& config) override;

  drogon::Task<std::expected<Json::Value, drogon::HttpResponsePtr>> fetchAllStamps();
  drogon::Task<std::expected<std::pair<std::string, std::string>, drogon::HttpResponsePtr>> getStampImage(std::string id);
  /// This method must be called by drogon to shutdown the plugin.
  /// It must be implemented by the user.
  void shutdown() override;
};


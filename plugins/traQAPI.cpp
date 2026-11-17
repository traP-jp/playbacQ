#include "traQAPI.h"

using namespace drogon;

void traQAPI::initAndStart(const Json::Value& config)
{
    if (const char* token = std::getenv("TRAQ_TOKEN"); token != nullptr) {
        Token = token;
    }
    if (const char* apiUrl = std::getenv("TRAQ_API_URL"); apiUrl != nullptr) {
        ApiUrl = apiUrl;
    }
    if (const char* botId = std::getenv("TRAQ_BOT_ID"); botId != nullptr) {
        BotId = botId;
    }
    if (Token.empty() || ApiUrl.empty() || BotId.empty()) {
        std::cerr << "TRAQ_TOKEN or TRAQ_API_URL or TRAQ_BOT_ID is not set. traQAPI plugin will not be initialized." << std::endl;
        return;
    }
    client = drogon::HttpClient::newHttpClient(ApiUrl);
}

drogon::Task<drogon::HttpResponsePtr> traQAPI::getFromTraQAPI(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params
) {
    co_return co_await callTraQAPI(drogon::Get, endpoint, params);
}

drogon::Task<drogon::HttpResponsePtr> traQAPI::postToTraQAPI(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params
) {
    co_return co_await callTraQAPI(drogon::Post, endpoint, params);
}

drogon::Task<drogon::HttpResponsePtr> traQAPI::callTraQAPI(
    const drogon::HttpMethod method,
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params
) {
    if (Token.empty()) {
        std::cerr << "TRAQ_TOKEN is not set. Cannot make request to traQ API." << std::endl;
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k500InternalServerError);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody("traQ Token is not set");
        co_return resp;
    }
    try {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(method);
        req->setPath(endpoint);
        req->addHeader("Authorization", std::format("Bearer {}", Token));
        for (const auto& [key, value] : params) {
            req->setParameter(key, value);
        }
        auto resp = co_await client->sendRequestCoro(req);

        if (resp->getStatusCode() != drogon::k200OK) {
            std::cerr << std::format("traQ API error ({}): {}",
                static_cast<int>(resp->getStatusCode()),
                resp->getBody()) << std::endl;
            auto errorResp = drogon::HttpResponse::newHttpResponse();
            errorResp->setStatusCode(resp->getStatusCode() >= 500
                ? drogon::k502BadGateway : resp->getStatusCode());
            errorResp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            errorResp->setBody("Failed to fetch data from traQ API");
            co_return errorResp;
        }
        co_return resp;
    }
    catch (const std::exception& e) {
        std::cerr << "Exception while calling traQ API: " << e.what() << std::endl;
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k502BadGateway);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody("Bad Gateway");
        co_return resp;
    }
}

drogon::Task<std::expected<Json::Value, drogon::HttpResponsePtr>> traQAPI::fetchAllStamps()
{
    {
        // キャッシュが有効であればキャッシュを返す
        std::shared_lock lock(mutex);
        if (!allStampsCache.stamps.empty() && std::chrono::steady_clock::now() - allStampsCache.timestamp < cacheDuration) {
            co_return allStampsCache.stamps;
        }
    }
    auto resp = co_await getFromTraQAPI("/api/v3/stamps");
    if (resp->getStatusCode() != drogon::k200OK) {
        std::cerr << "Error fetching stamps: " << resp->getBody() << std::endl;
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k500InternalServerError);
        response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        response->setBody(std::format("Error fetching stamps: {}", resp->getBody()));
        co_return std::unexpected(response);
    }

    auto jsonPtr = resp->getJsonObject();
    if (!jsonPtr) {
        std::cerr << "Error parsing JSON response from traQ API" << std::endl;
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k500InternalServerError);
        response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        response->setBody("Error parsing JSON response from traQ API");
        co_return std::unexpected(response);
    }
    if (!jsonPtr->isArray()) {
        std::cerr << "Unexpected JSON format from traQ API" << std::endl;
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k500InternalServerError);
        response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        response->setBody("Unexpected JSON format from traQ API");
        co_return std::unexpected(response);
    }
    Json::Value result_array(Json::arrayValue);
    for (const auto& stamp : *jsonPtr) {
        Json::Value stamp_info;
        stamp_info["id"] = stamp.get("id", "").asString();
        stamp_info["name"] = stamp.get("name", "").asString();
        result_array.append(std::move(stamp_info));
    }
    // キャッシュに保存
    std::unique_lock lock(mutex);
    allStampsCache = { result_array, std::chrono::steady_clock::now() };
    co_return result_array;
}

drogon::Task<std::expected<std::pair<std::string, std::string>, drogon::HttpResponsePtr>> traQAPI::getStampImage(std::string id)
{
    {
        // キャッシュが有効であればキャッシュを返す
        std::shared_lock lock(mutex);
        auto it = stampImageCache.find(id);
        if (it != stampImageCache.end()) {
            if (std::chrono::steady_clock::now() - it->second.timestamp < cacheDuration) {
                it->second.lastAccessed.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);
                co_return std::make_pair(it->second.imageRawData, it->second.contentType);
            }
        }
    }

    auto resp = co_await getFromTraQAPI("/api/v3/stamps/" + id + "/image");
    if (resp->getStatusCode() != drogon::k200OK) {
        std::cerr << "Error fetching stamp image: " << resp->getBody() << std::endl;
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k500InternalServerError);
        response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        response->setBody(std::format("Error fetching stamp image: {}", resp->getBody()));
        co_return std::unexpected(response);
    }
    std::string imageData = std::string(resp->getBody());
    // キャッシュに保存
    std::string contentType = resp->getHeader("Content-Type");
    if (contentType.empty()) {
        contentType = "image/png";
    }
    {
        std::unique_lock lock(mutex);
        auto now = std::chrono::steady_clock::now();
        if (auto it = stampImageCache.find(id); it != stampImageCache.end()) {
            // キャッシュ期限切れの場合、上書き
            it->second.imageRawData = imageData;
            it->second.contentType = contentType;
            it->second.timestamp = now;
            it->second.lastAccessed.store(now, std::memory_order_relaxed);
        } else {
            // 新規追加
            stampImageCache.try_emplace(id, imageData, contentType, now, now);
        }
        while (stampImageCache.size() * sizeof(stampImageCacheEntry) > maxStampImageCacheSize) {
            // キャッシュサイズが上限を超えた場合、最も古いアクセスのスタンプ画像を削除する
            auto oldest = std::min_element(stampImageCache.begin(), stampImageCache.end(),
                [](const auto& a, const auto& b) {
                    return a.second.lastAccessed.load(std::memory_order_relaxed) < b.second.lastAccessed.load(std::memory_order_relaxed);
                });
            if (oldest != stampImageCache.end()) {
                stampImageCache.erase(oldest);
            }
        }
    }
    co_return std::make_pair(std::move(imageData), std::move(contentType));
}

drogon::Task<drogon::HttpResponsePtr> traQAPI::getChannels(std::string query)
{
    // queryが安全か確認
    if ([&query]() -> bool {
        if (query.empty()) return false;
        // 512文字以上のチャンネルは存在しないはず
            if (query.size() > 512) return true;
            return !std::ranges::none_of(query, [](unsigned char c) {
                return std::iscntrl(c);
                });
        }()) {
        auto badReq = drogon::HttpResponse::newHttpResponse();
        badReq->setStatusCode(drogon::k400BadRequest);
        badReq->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        badReq->setBody("Invalid query parameter");
        co_return badReq;
    }

    co_return co_await getFromTraQAPI("/api/v3/channels", {
        { "include-dm", "false" },
        { "path", query }
        });
}

drogon::Task<bool> traQAPI::channelExists(std::string_view channelId)
{
    auto resp = co_await getFromTraQAPI(std::format("/api/v3/channels/{}", channelId));
    co_return resp->getStatusCode() == drogon::k200OK;
}

drogon::Task<void> traQAPI::joinChannel(std::string_view channelId)
{

    auto resp = co_await postToTraQAPI(std::format("/api/v3/bots/{}/actions/join", BotId), {
        { "channelId", channelId.data() }
        });
    if (resp->getStatusCode() != drogon::k200OK)
        throw std::runtime_error(std::format("Failed to join channel: {}. Error: {}", channelId, resp->getBody()));
    co_return;
}

drogon::Task<void> traQAPI::leaveChannel(std::string_view channelId)
{
    auto resp = co_await postToTraQAPI(std::format("/api/v3/bots/{}/actions/leave", BotId), {
        { "channelId", channelId.data() }
        });
    if (resp->getStatusCode() != drogon::k200OK)
        throw std::runtime_error(std::format("Failed to leave channel: {}. Error: {}", channelId, resp->getBody()));
    co_return;
}

void traQAPI::shutdown()
{
    /// Shutdown the plugin
}

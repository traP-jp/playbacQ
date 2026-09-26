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
    if (Token.empty() || ApiUrl.empty()) {
        std::cerr << "TRAQ_TOKEN or TRAQ_API_URL is not set. traQAPI plugin will not be initialized." << std::endl;
        return;
    }
    client = drogon::HttpClient::newHttpClient(ApiUrl);
}

drogon::Task<drogon::HttpResponsePtr> traQAPI::getFromTraQAPI(const std::string& endpoint)
{
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
        req->setMethod(drogon::Get);
        req->setPath(endpoint);
        req->addHeader("Authorization", std::format("Bearer {}", Token));
        auto resp = co_await client->sendRequestCoro(req);

        if (resp->getStatusCode() != drogon::k200OK) {
            std::cerr << "Error fetching stamps: " << resp->getBody() << std::endl;
            auto errorResp = drogon::HttpResponse::newHttpResponse();
            errorResp->setStatusCode(drogon::k500InternalServerError);
            errorResp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            errorResp->setBody(std::format("Error fetching stamps: {}", resp->getBody()));
            co_return errorResp;
        }
        co_return resp;
    }
    catch (const std::exception& e) {
        std::cerr << "Error fetching stamps: " << e.what() << std::endl;
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k500InternalServerError);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody(std::string("Error fetching stamps: ") + e.what());
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
        if (it != stampImageCache.end() && std::chrono::steady_clock::now() - it->second.timestamp < cacheDuration) {
            it->second.lastAccessed = std::chrono::steady_clock::now();
            co_return std::make_pair(it->second.imageRawData, it->second.contentType);
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
    std::unique_lock lock(mutex);
    std::string contentType = resp->getHeader("Content-Type");
    if (contentType.empty()) {
        contentType = "image/png";
    }
    stampImageCache[id] = { imageData, contentType, std::chrono::steady_clock::now(), std::chrono::steady_clock::now() };
    if (stampImageCache.size() > maxStampImageCacheSize) {
        // キャッシュサイズが上限を超えた場合、最も古いアクセスのスタンプ画像を削除する
        auto oldest = std::min_element(stampImageCache.begin(), stampImageCache.end(),
            [](const auto& a, const auto& b) {
                return a.second.lastAccessed < b.second.lastAccessed;
            });
        if (oldest != stampImageCache.end()) {
            stampImageCache.erase(oldest);
        }
    }
    co_return std::make_pair(imageData, contentType);
}

void traQAPI::shutdown()
{
    /// Shutdown the plugin
}

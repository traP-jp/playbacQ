#include "api_auth.h"

#include <cstdlib>
#include <string_view>

using namespace api;

namespace {

constexpr std::string_view kDefaultFrontendUrl = "https://playbacq.trap.show";
constexpr std::string_view kDefaultRedirectPath = "/";

bool isSafeRedirectPath(std::string_view redirectPath) {
    if (redirectPath.empty() || redirectPath.front() != '/') {
        return false;
    }
    if (redirectPath.size() > 1 && redirectPath[1] == '/') {
        return false;
    }
    for (const unsigned char character : redirectPath) {
        if (character < 0x20 || character == 0x7f || character == '\\') {
            return false;
        }
    }
    return true;
}

std::string getFrontendUrl() {
    const char* configuredUrl = std::getenv("FRONTEND_URL");
    std::string frontendUrl = configuredUrl != nullptr && configuredUrl[0] != '\0'
        ? std::string(configuredUrl)
        : std::string(kDefaultFrontendUrl);
    while (!frontendUrl.empty() && frontendUrl.back() == '/') {
        frontendUrl.pop_back();
    }
    return frontendUrl;
}

} // namespace

drogon::Task<drogon::HttpResponsePtr> auth::login(HttpRequestPtr req) {
    auto redirectUrl = req->getOptionalParameter<std::string>("redirect");
    std::string_view redirectPath = redirectUrl.has_value() && isSafeRedirectPath(*redirectUrl)
        ? std::string_view(*redirectUrl)
        : kDefaultRedirectPath;
    std::string target = getFrontendUrl() + std::string(redirectPath);
    auto resp = drogon::HttpResponse::newRedirectionResponse(target);
    co_return resp;
}

drogon::Task<drogon::HttpResponsePtr> auth::getUser(HttpRequestPtr req) {
    try {
        std::string userId = req->getAttributes()->get<std::string>("userId");
        Json::Value jsonResponse;
        jsonResponse["userId"] = userId;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(jsonResponse);
        resp->setStatusCode(drogon::HttpStatusCode::k200OK);
        co_return resp;
    }
    catch (const std::exception& e) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
        resp->setBody("Failed to retrieve user: " + std::string(e.what()));
        co_return resp;
    }
}

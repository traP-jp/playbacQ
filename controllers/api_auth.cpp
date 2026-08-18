#include "api_auth.h"

#include <cstdlib>
#include <string_view>

using namespace api;

namespace {

constexpr std::string_view kDefaultFrontendUrl = "http://localhost:4200";
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

std::string normalizeRedirectPath(std::string_view redirect, std::string_view frontendUrl) {
    if (isSafeRedirectPath(redirect)) {
        return std::string(redirect);
    }

    if (frontendUrl.empty()) {
        return std::string(kDefaultRedirectPath);
    }

    if (redirect == frontendUrl) {
        return std::string(kDefaultRedirectPath);
    }

    const bool hasSameFrontendPrefix = redirect.size() > frontendUrl.size()
        && redirect.compare(0, frontendUrl.size(), frontendUrl) == 0
        && redirect[frontendUrl.size()] == '/';
    if (!hasSameFrontendPrefix) {
        return std::string(kDefaultRedirectPath);
    }

    const std::string_view redirectPath = redirect.substr(frontendUrl.size());
    return isSafeRedirectPath(redirectPath)
        ? std::string(redirectPath)
        : std::string(kDefaultRedirectPath);
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
    const std::string frontendUrl = getFrontendUrl();
    const std::string redirectPath = redirectUrl.has_value()
        ? normalizeRedirectPath(*redirectUrl, frontendUrl)
        : std::string(kDefaultRedirectPath);
    std::string target = frontendUrl + redirectPath;
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

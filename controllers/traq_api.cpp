#include "traq_api.h"
#include <json/json.h>
#include "../plugins/traQAPI.h"

drogon::Task<drogon::HttpResponsePtr> traq_api::fetchAllStamps([[maybe_unused]] HttpRequestPtr req) {
    auto* traqAPI = drogon::app().getPlugin<traQAPI>();
    if (!traqAPI) {
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k500InternalServerError);
        response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        response->setBody("traQAPI plugin is not initialized");
        co_return response;
    }
    auto result = co_await traqAPI->fetchAllStamps();
    if (!result.has_value()) {
        co_return result.error();
    }
    co_return drogon::HttpResponse::newHttpJsonResponse(result.value());
}

drogon::Task<drogon::HttpResponsePtr> traq_api::getStampImage([[maybe_unused]] HttpRequestPtr req, std::string id) {
    auto* traqAPI = drogon::app().getPlugin<traQAPI>();
    if (!traqAPI) {
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k500InternalServerError);
        response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        response->setBody("traQAPI plugin is not initialized");
        co_return response;
    }
    auto result = co_await traqAPI->getStampImage(id);
    if (!result.has_value()) {
        co_return result.error();
    }
    co_return drogon::HttpResponse::newHttpJsonResponse(result.value());
}

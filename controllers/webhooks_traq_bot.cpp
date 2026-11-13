#include "webhooks_traq_bot.h"
#include <drogon/HttpClient.h>
#include <drogon/orm/Exception.h>
#include <drogon/orm/CoroMapper.h>
#include <drogon/orm/Criteria.h>
#include <json/json.h>
#include "../plugins/BotPlugin.h"

using namespace webhooks;

drogon::Task<drogon::HttpResponsePtr> traQbot::asyncHandleHttpRequest(HttpRequestPtr req) {
	auto token = req->getHeader("X-TRAQ-BOT-TOKEN");
	const char* expectedToken = std::getenv("TRAQ_BOT_TOKEN");
	std::string expectedTokenStr = expectedToken ? expectedToken : "";
	if (token != expectedTokenStr) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k403Forbidden);
		resp->setBody("Invalid token");
		co_return resp;
	}
	auto eventType = req->getHeader("X-TRAQ-BOT-EVENT");
	auto body = jsonPtr->toStyledString();
	if (eventType == "PING" || eventType == "JOINED" || eventType == "LEFT") {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k204NoContent);
		co_return resp;
	}
	auto* botPlugin = drogon::app().getPlugin<BotPlugin>();
	if (eventType == "MESSAGE_CREATED") {
		// TODO
	} else if (eventType == "MESSAGE_UPDATED") {
		// TODO
	} else if (eventType == "MESSAGE_DELETED") {
		// TODO
	}

	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
	resp->setBody("Unsupported event type: " + eventType);
	co_return resp;
}
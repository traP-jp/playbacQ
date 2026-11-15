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
	const auto body = req->getJsonObject();
	if (eventType == "PING" || eventType == "JOINED" || eventType == "LEFT") {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k204NoContent);
		co_return resp;
	}
	if (!body->isMember("message")) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
		resp->setBody("Missing 'message' field in request body");
		co_return resp;
	}
	const auto message = (*body)["message"];
	const std::string content = message.get("plainText", "").asString();
	const std::string messageId = message.get("id", "").asString();
	auto* botPlugin = drogon::app().getPlugin<BotPlugin>();
	if (eventType == "MESSAGE_CREATED") {
		const std::string channelId = message.get("channel", "").asString();
		const std::string createdAt = message.get("createdAt", "").asString();
		co_return co_await botPlugin->postComment(channelId, content, createdAt, messageId);
	} else if (eventType == "MESSAGE_UPDATED") {
		co_return co_await botPlugin->updateComment(content, messageId);
	} else if (eventType == "MESSAGE_DELETED") {
		co_return co_await botPlugin->deleteComment(messageId);
	}

	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
	resp->setBody("Unsupported event type: " + eventType);
	co_return resp;
}
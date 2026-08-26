#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class traq_api : public drogon::HttpController<traq_api>
{
public:
  METHOD_LIST_BEGIN;
  ADD_METHOD_TO(traq_api::fetchAllStamps, "/traq-api/stamps", Get, "AuthFilter");
  ADD_METHOD_TO(traq_api::getStampImage, "/traq-api/stamps/{1}/image", Get, "AuthFilter");
  METHOD_LIST_END;
  drogon::Task<drogon::HttpResponsePtr> fetchAllStamps(HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> getStampImage(HttpRequestPtr req, std::string id);
};

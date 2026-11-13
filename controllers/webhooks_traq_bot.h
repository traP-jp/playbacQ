#pragma once

#include <drogon/utils/coroutine.h>
#include <drogon/HttpController.h>
using namespace drogon;

namespace webhooks
{
  class traQbot : public drogon::HttpController<traQbot> {
  public:
    METHOD_LIST_BEGIN;
    ADD_METHOD_TO(traQbot::asyncHandleHttpRequest, "/webhooks/traq", Post);
    METHOD_LIST_END;
    drogon::Task<drogon::HttpResponsePtr> asyncHandleHttpRequest(HttpRequestPtr req);
  };
}

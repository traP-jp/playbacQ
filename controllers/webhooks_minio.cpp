#include "webhooks_minio.h"
#include <drogon/HttpClient.h>
#include <drogon/orm/Exception.h>
#include <drogon/orm/CoroMapper.h>
#include <drogon/orm/Criteria.h>
#include <json/json.h>
#include <boost/process.hpp>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include "../models/Videos.h"
#include "Status.h"

using namespace webhooks;

//【重要！】：200 OK以外を返すとMinIOが永遠とリトライするため、エラーがあっても200 OK以外は返さないこと！
drogon::Task<drogon::HttpResponsePtr> minio::asyncHandleHttpRequest(HttpRequestPtr req) {
    auto jsonPtr = req->getJsonObject();
    std::cout << "Start processing MinIO webhook" << std::endl;
    if (!jsonPtr) {
        std::cerr << "Invalid JSON format in request body" << std::endl;
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::HttpStatusCode::k200OK);
        resp->setBody("Invalid JSON format");
        co_return resp;
    }
    // Records配列の整合性チェック
    if (!jsonPtr->isMember("Records") || !(*jsonPtr)["Records"].isArray() || (*jsonPtr)["Records"].empty()) {
        std::cerr << "Missing or invalid 'Records' field in request body" << std::endl;
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::HttpStatusCode::k200OK);
        resp->setBody("Missing or invalid 'Records' field");
        co_return resp;
    }
    for (const auto& record : (*jsonPtr)["Records"]) {
        if (!record.isMember("eventName") || !record["eventName"].isString()) {
            std::cerr << "Missing or invalid 'eventName' field in record" << std::endl;
            continue;
        }
        if (!record.isMember("s3") || !record["s3"].isObject()
            || !record["s3"].isMember("object") || !record["s3"]["object"].isObject()
            || !record["s3"]["object"].isMember("key") || !record["s3"]["object"]["key"].isString()) {
            std::cerr << "Missing or invalid 's3.object.key' field in record" << std::endl;
            continue;
        }
        std::string eventName = record.get("eventName", "NULL").asString();
        std::string videoFileName = record["s3"]["object"].get("key", "NULL").asString();
        if (eventName == "NULL" || videoFileName == "NULL") {
            std::cerr << "Invalid " << (eventName == "NULL" ? "eventName" : "key") << " in record" << std::endl;
            continue;
        }
        // 投げられたイベントとVideoIDをキューに積む
        std::string videoId = videoFileName.substr(0, videoFileName.find_last_of('.'));
        if (eventName.starts_with("s3:ObjectCreated:")) {
            drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
            try {
                // ステータスを更新
                auto video = co_await mapper.findByPrimaryKey(videoId);
                video.setStatus((uint8_t)Status::processing);
                co_await mapper.update(video);
                std::cout << "Dispatching video ID " << videoId << " for encoding" << std::endl;
#ifdef USE_LOCAL_WORKER
                std::thread([videoId]() {
                    try {
                        const char* workerExecutableEnv = std::getenv("LOCAL_WORKER_EXECUTABLE");
                        const std::string workerExecutable = workerExecutableEnv
                            ? workerExecutableEnv
                            : "./build/playbacq_worker";
                        std::string resolvedWorkerExecutable = workerExecutable;
                        if (!std::filesystem::exists(resolvedWorkerExecutable)) {
                            const auto workerPath = boost::process::search_path(workerExecutable);
                            if (!workerPath.empty()) {
                                resolvedWorkerExecutable = workerPath.string();
                            }
                        }
                        if (!std::filesystem::exists(resolvedWorkerExecutable)) {
                            throw std::runtime_error("Local worker executable not found: " + workerExecutable);
                        }
                        std::cout << "Starting local worker " << resolvedWorkerExecutable
                                  << " for video ID " << videoId << std::endl;
                        boost::process::child c(resolvedWorkerExecutable, videoId);
                        c.detach();
                    }
                    catch (const std::exception& e) {
                        std::cerr << "Error starting playbacq_worker for video ID " << videoId << ": " << e.what() << std::endl;
                    }
                    }).detach();
#else
                // Modalにpostする
                const char* modalHost_env = std::getenv("MODAL_HOST");
                std::string modalHost = modalHost_env ? modalHost_env : "http://localhost:9999";
                auto client = drogon::HttpClient::newHttpClient(modalHost);
                Json::Value payload;
                payload["video_id"] = videoId;
                auto req = drogon::HttpRequest::newHttpJsonRequest(payload);
                req->setMethod(drogon::HttpMethod::Post);
                req->setPath("/");

                try {
                    auto resp = co_await client->sendRequestCoro(req);
                    if (resp->getStatusCode() == drogon::HttpStatusCode::k200OK) {
                        std::cout << "Successfully pushed video ID " << videoId << " to Modal" << std::endl;
                    } else {
                        std::cerr << "Failed to push video ID " << videoId << " to Modal. HTTP Status: " << resp->getStatusCode() << std::endl;
                    }
                }
                catch (const std::exception& e) {
                    std::cerr << "Error sending request to Modal: " << e.what() << std::endl;
                }
#endif
            }
            catch (const drogon::orm::DrogonDbException& e) {
                std::cerr << "DB Error: " << e.base().what() << std::endl;
            }
            catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
        } else {
            // 現時点ではアップロード完了通知以外はエラー扱い。
            std::cerr << "Unsupported event: " << eventName << std::endl;
        }
    }
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::HttpStatusCode::k200OK);
    resp->setBody("Finished processing webhook");
    co_return resp;
}

drogon::Task<drogon::HttpResponsePtr> minio::receiveEncodeResult(HttpRequestPtr req) {
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        std::cerr << "Invalid JSON format in request body" << std::endl;
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::HttpStatusCode::k200OK);
        resp->setBody("Invalid JSON format");
        co_return resp;
    }
    if (!jsonPtr->isMember("video_id") || !jsonPtr->isMember("status") || !jsonPtr->isMember("message")
        || !(*jsonPtr)["video_id"].isString() || !(*jsonPtr)["status"].isString() || !(*jsonPtr)["message"].isString()) {
        std::cerr << "Missing or invalid 'video_id' or 'status' or 'message' field in request body" << std::endl;
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::HttpStatusCode::k200OK);
        resp->setBody("Missing or invalid 'video_id' or 'status' or 'message' field");
        co_return resp;
    }
    std::string videoId = (*jsonPtr)["video_id"].asString();
    std::string status = (*jsonPtr)["status"].asString();
    std::string message = (*jsonPtr)["message"].asString();
    int duration = (*jsonPtr).get("duration", 0).asInt();
    if (status == "completed" || status == "failed") {
        drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
        try {
            auto video = co_await mapper.findByPrimaryKey(videoId);
            if (status == "completed") {
                video.setStatus((uint8_t)Status::completed);
                video.setDuration(duration);
            } else {
                video.setStatus((uint8_t)Status::failed);
            }
            co_await mapper.update(video);
        }
        catch (const drogon::orm::DrogonDbException& e) {
            std::cerr << "DB Error: " << e.base().what() << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    } else {
        std::cerr << "Unsupported status: " << status << std::endl;
    }
    std::cout << "Received encode result for video ID: " << videoId << ", status: " << status << ", message: " << message << std::endl;
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::HttpStatusCode::k200OK);
    co_return resp;
}

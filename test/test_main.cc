#define DROGON_TEST_MAIN
#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <condition_variable>

// Modalモックサーバーが受信したvideo_idを記録
std::mutex g_modalMutex;
std::vector<std::string> g_modalReceivedVideoIds;
std::condition_variable g_modalCv;
std::atomic<unsigned int> g_youtubeVideosListCalls{ 0 };
std::mutex g_youtubeRequestMutex;
std::string g_lastYoutubeVideoParts;
std::string g_lastYoutubeVideoIds;
std::mutex g_youtubeLiveInfoMutex;
std::optional<std::string> g_mockActualStartTime = std::nullopt;
std::optional<std::string> g_mockActualEndTime = std::nullopt;
std::mutex g_traqLeaveMutex;
std::condition_variable g_traqLeaveCv;
std::atomic<unsigned int> g_traqLeaveCalls{ 0 };

int main(int argc, char** argv)
{
    using namespace drogon;

    std::promise<void> p1;
    std::future<void> f1 = p1.get_future();

#ifndef USE_INTERNAL_S3
    std::cerr << "WARNING: Using external S3 endpoint. Make sure the test MinIO server is running and accessible at " << std::getenv("S3_ENDPOINT") << std::endl;
#endif

    // Start the main loop on another thread
    std::thread thr([&]() {
        // Queues the promise to be fulfilled after starting the loop
        drogon::app().addListener("127.0.0.1", 8080);
        // Modalモックサーバー用
        drogon::app().addListener("127.0.0.1", 9999);
        drogon::orm::MysqlConfig config;
        config.host = "test-db";
        config.port = 3306;
        config.databaseName = "playbacq_test";
        config.username = "test_user";
        config.password = "test_pass";
        config.connectionNumber = 3;
        config.name = "default";
        drogon::app().addDbClient(config);

        // Modalモックハンドラ: POST / で受信したvideo_idを記録
        drogon::app().registerHandler(
            "/",
            [](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                    auto json = req->getJsonObject();
                    if (json && json->isMember("video_id")) {
                        std::lock_guard<std::mutex> lock(g_modalMutex);
                        g_modalReceivedVideoIds.push_back((*json)["video_id"].asString());
                        g_modalCv.notify_all();
                    }
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k200OK);
                    callback(resp);
            },
            { drogon::Post });

        // YouTube videos.list モック
        drogon::app().registerHandler(
            "/youtube/v3/videos",
            [](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                    g_youtubeVideosListCalls.fetch_add(1, std::memory_order_relaxed);
                    const std::string videoId = req->getParameter("id");
                    const std::string part = req->getParameter("part");
                    {
                        std::lock_guard<std::mutex> lock(g_youtubeRequestMutex);
                        g_lastYoutubeVideoParts = part;
                        g_lastYoutubeVideoIds = videoId;
                    }
                    // Live Info 用
                    if (part == "liveStreamingDetails") {
                        if (videoId == "FAILVIDEO01") {
                            auto error = drogon::HttpResponse::newHttpResponse();
                            error->setStatusCode(drogon::k500InternalServerError);
                            callback(error);
                            return;
                        }
                        Json::Value body;
                        body["items"] = Json::Value(Json::arrayValue);
                        Json::Value item;
                        item["id"] = videoId;
                        // TEST側で設定した値を使う
                        std::lock_guard<std::mutex> lock(g_youtubeLiveInfoMutex);
                        if (g_mockActualStartTime.has_value()) {
                            item["liveStreamingDetails"]["actualStartTime"] = *g_mockActualStartTime;
                        }
                        if (g_mockActualEndTime.has_value()) {
                            item["liveStreamingDetails"]["actualEndTime"] = *g_mockActualEndTime;
                        }
                        body["items"].append(std::move(item));
                        callback(drogon::HttpResponse::newHttpJsonResponse(body));
                        return;
                    }

                    if (videoId == "FAILVIDEO01") {
                        auto error = drogon::HttpResponse::newHttpResponse();
                        error->setStatusCode(drogon::k500InternalServerError);
                        callback(error);
                        return;
                    }

                    Json::Value body;
                    body["items"] = Json::Value(Json::arrayValue);
                    if (videoId == "MISSVIDEO01") {
                        callback(drogon::HttpResponse::newHttpJsonResponse(body));
                        return;
                    }

                    const bool partialVideo = videoId == "PARTVIDEO01";
                    const bool activeLive = videoId == "LIVEVIDEO01";
                    const bool upcomingPremiere = videoId == "PREMVIDEO01";
                    const bool dayLongVideo = videoId == "DAYVIDEO001";

                    Json::Value item;
                    item["id"] = videoId;
                    item["snippet"]["title"] = "YouTube同期タイトル";
                    if (!partialVideo) {
                        item["snippet"]["description"] = "YouTube同期説明";
                    }
                    item["snippet"]["liveBroadcastContent"] = activeLive
                        ? "live"
                        : upcomingPremiere ? "upcoming" : "none";
                    item["contentDetails"]["duration"] = activeLive
                        ? "PT0S"
                        : upcomingPremiere ? "PT1H"
                        : dayLongVideo ? "P1DT2H3M4S" : "PT2M3S";
                    body["items"].append(std::move(item));
                    callback(drogon::HttpResponse::newHttpJsonResponse(body));
            },
            { drogon::Get });

        // traQ API スタンプ一覧モック: GET /api/v3/stamps
        drogon::app().registerHandler(
            "/api/v3/stamps",
            [](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                    Json::Value body(Json::arrayValue);
                    Json::Value stamp1;
                    stamp1["id"] = "stamp-id-001";
                    stamp1["name"] = "sample_stamp_1";
                    stamp1["creatorId"] = "user-id-001";
                    body.append(stamp1);

                    Json::Value stamp2;
                    stamp2["id"] = "stamp-id-002";
                    stamp2["name"] = "sample_stamp_2";
                    stamp2["creatorId"] = "user-id-002";
                    body.append(stamp2);

                    callback(drogon::HttpResponse::newHttpJsonResponse(body));
            },
            { drogon::Get });

        // traQ API スタンプ画像取得モック: GET /api/v3/stamps/{id}/image
        drogon::app().registerHandler(
            "/api/v3/stamps/{stampId}/image",
            [](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                const std::string& stampId) {
                    if (stampId == "not-found-stamp") {
                        auto resp = drogon::HttpResponse::newHttpResponse();
                        resp->setStatusCode(drogon::k404NotFound);
                        resp->setBody("Stamp not found");
                        callback(resp);
                        return;
                    }

                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k200OK);
                    resp->setContentTypeCode(drogon::CT_CUSTOM);
                    resp->setContentTypeString("image/png");
                    resp->setBody("DUMMY_PNG_DATA_FOR_" + stampId);
                    callback(resp);
            },
            { drogon::Get });
        // traQ API チャンネル一覧取得モック: GET /api/v3/channels
        drogon::app().registerHandler(
            "/api/v3/channels",
            [](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                    Json::Value body(Json::arrayValue);
                    Json::Value channel1;
                    channel1["id"] = "channel-id-001";
                    channel1["name"] = "sample_channel_1";
                    body.append(channel1);

                    Json::Value channel2;
                    channel2["id"] = "channel-id-002";
                    channel2["name"] = "sample_channel_2";
                    body.append(channel2);

                    callback(drogon::HttpResponse::newHttpJsonResponse(body));
            }
        , { drogon::Get });

        app().loadConfigFile("config.test.json");
        app().getLoop()->queueInLoop([&p1]() { p1.set_value(); });
        app().run();
        });
    // traQ API チャンネル参加モック: POST /api/v3/bots/{botId}/actions/join
    drogon::app().registerHandler(
        "/api/v3/bots/{botId}/actions/join",
        [](const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback,
            const std::string& botId) {
                const char* botIdEnv = std::getenv("TRAQ_BOT_ID");
                const std::string expectedBotId = botIdEnv ? botIdEnv : "";
                if (botId != expectedBotId) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k403Forbidden);
                    resp->setBody("Failed to join channel");
                    callback(resp);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(g_traqLeaveMutex);
                    g_traqLeaveCalls.fetch_add(1);
                }
                // テストに完了通知を投げる
                g_traqLeaveCv.notify_all();

                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                callback(resp);
        },
        { drogon::Post });
    // traQ API チャンネル退室モック: POST /api/v3/bots/{botId}/actions/leave
    drogon::app().registerHandler(
        "/api/v3/bots/{botId}/actions/leave",
        [](const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback,
            const std::string& botId) {
                const char* botIdEnv = std::getenv("TRAQ_BOT_ID");
                const std::string expectedBotId = botIdEnv ? botIdEnv : "";
                if (botId != expectedBotId) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k403Forbidden);
                    resp->setBody("Failed to leave channel");
                    callback(resp);
                    return;
                }
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                callback(resp);
        },
        { drogon::Post });

    // The future is only satisfied after the event loop started
    f1.get();
    int status = test::run(argc, argv);

    // Ask the event loop to shutdown and wait
    app().getLoop()->queueInLoop([]() { app().quit(); });
    thr.join();
    return status;
}

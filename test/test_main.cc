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
std::atomic<unsigned int> g_youtubeVideosListCalls{0};
std::mutex g_youtubeRequestMutex;
std::string g_lastYoutubeVideoParts;
std::string g_lastYoutubeVideoIds;

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
            {drogon::Post});

        // YouTube videos.list モック
        drogon::app().registerHandler(
            "/youtube/v3/videos",
            [](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                g_youtubeVideosListCalls.fetch_add(1, std::memory_order_relaxed);
                const std::string videoId = req->getParameter("id");
                {
                    std::lock_guard<std::mutex> lock(g_youtubeRequestMutex);
                    g_lastYoutubeVideoParts = req->getParameter("part");
                    g_lastYoutubeVideoIds = videoId;
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
            {drogon::Get});

        app().loadConfigFile("config.test.json");
        app().getLoop()->queueInLoop([&p1]() { p1.set_value(); });
        app().run();
        });

    // The future is only satisfied after the event loop started
    f1.get();
    int status = test::run(argc, argv);

    // Ask the event loop to shutdown and wait
    app().getLoop()->queueInLoop([]() { app().quit(); });
    thr.join();
    return status;
}

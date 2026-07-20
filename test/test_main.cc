#define DROGON_TEST_MAIN
#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <mutex>
#include <vector>
#include <condition_variable>

// Modalモックサーバーが受信したvideo_idを記録
std::mutex g_modalMutex;
std::vector<std::string> g_modalReceivedVideoIds;
std::condition_variable g_modalCv;

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

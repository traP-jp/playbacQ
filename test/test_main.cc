#define DROGON_TEST_MAIN
#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <atomic>
#include <mutex>
#include <string>
#include <sstream>
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
				{
					std::lock_guard<std::mutex> lock(g_youtubeRequestMutex);
					g_lastYoutubeVideoParts = req->getParameter("part");
					g_lastYoutubeVideoIds = req->getParameter("id");
				}
				if (req->getParameter("id").find("FAILVIDEO01") != std::string::npos) {
					auto error = drogon::HttpResponse::newHttpResponse();
					error->setStatusCode(drogon::k500InternalServerError);
					callback(error);
					return;
				}

				Json::Value body;
				body["items"] = Json::Value(Json::arrayValue);
				std::stringstream ids(req->getParameter("id"));
				std::string id;
				while (std::getline(ids, id, ',')) {
					if (id == "MISSVIDEO01") {
						continue;
					}
					const bool secondVideo = id == "MOCKVIDEO02";
					const bool partialVideo = id == "PARTVIDEO01";
					Json::Value item;
					item["kind"] = "youtube#video";
					item["etag"] = "public-etag";
					item["id"] = id;
					item["snippet"]["publishedAt"] = "2026-08-22T00:00:00Z";
					item["snippet"]["channelId"] = "public-channel";
					item["snippet"]["title"] = secondVideo ? "YouTube同期タイトル2" : "YouTube同期タイトル";
					if (!partialVideo) {
						item["snippet"]["description"] = secondVideo ? "YouTube同期説明2" : "YouTube同期説明";
					}
					item["snippet"]["thumbnails"]["high"]["url"] = "https://example.test/high.jpg";
					item["snippet"]["channelTitle"] = "公開チャンネル";
					item["snippet"]["tags"].append("公開タグ");
					item["snippet"]["categoryId"] = "22";
					item["snippet"]["liveBroadcastContent"] = "none";
					item["contentDetails"]["duration"] = secondVideo ? "PT3M4S" : "PT2M3S";
					item["contentDetails"]["dimension"] = "2d";
					item["contentDetails"]["definition"] = "hd";
					item["contentDetails"]["caption"] = "true";
					item["contentDetails"]["licensedContent"] = true;
					item["contentDetails"]["projection"] = "rectangular";
					item["contentDetails"]["hasCustomThumbnail"] = true;
					item["status"]["privacyStatus"] = "public";
					item["status"]["license"] = "youtube";
					item["status"]["embeddable"] = true;
					item["status"]["madeForKids"] = false;
					item["status"]["selfDeclaredMadeForKids"] = false;
					item["statistics"]["viewCount"] = secondVideo ? "2234" : "1234";
					if (!partialVideo) {
						item["statistics"]["likeCount"] = secondVideo ? "66" : "56";
					}
					item["statistics"]["commentCount"] = secondVideo ? "88" : "78";
					item["statistics"]["favoriteCount"] = "0";
					item["statistics"]["dislikeCount"] = "9";
					item["paidProductPlacementDetails"]["hasPaidProductPlacement"] = false;
					item["player"]["embedHtml"] = "<iframe></iframe>";
					item["topicDetails"]["topicCategories"].append("https://en.wikipedia.org/wiki/Technology");
					item["recordingDetails"]["recordingDate"] = "2026-08-21T00:00:00Z";
					item["liveStreamingDetails"]["actualStartTime"] = "2026-08-21T00:00:00Z";
					item["brandPartner"]["channelId"] = "UCbrand";
					item["localizations"]["ja"]["title"] = "日本語タイトル";
					item["localizations"]["ja"]["description"] = "日本語説明";
					item["fileDetails"]["fileName"] = "owner-only.mp4";
					item["processingDetails"]["processingStatus"] = "succeeded";
					item["suggestions"]["processingWarnings"].append("owner-only");
					body["items"].append(std::move(item));
				}
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

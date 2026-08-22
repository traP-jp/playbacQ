#include <drogon/drogon_test.h>
#include <drogon/HttpClient.h>
#include <drogon/HttpTypes.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <drogon/drogon.h>
#include <drogon/nosql/RedisClient.h>
#include <drogon/WebSocketClient.h>
#include <future>
#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <vector>
#include "../controllers/websocket_comments.h"

extern std::atomic<unsigned int> g_youtubeVideosListCalls;
extern std::mutex g_youtubeRequestMutex;
extern std::string g_lastYoutubeVideoParts;
extern std::string g_lastYoutubeVideoIds;

drogon::HttpResponsePtr sendSyncRequest(
	drogon::HttpMethod method,
	const std::string& path,
	const Json::Value& body = Json::Value::null,
	const std::unordered_map<std::string, std::string>& queries = {},
	const std::string& authUser = "testuser"
) {
	auto client = drogon::HttpClient::newHttpClient("http://127.0.0.1:8080");
	auto req = drogon::HttpRequest::newHttpRequest();
	if (!body.isNull()) {
		req = drogon::HttpRequest::newHttpJsonRequest(body);
	} else {
		req = drogon::HttpRequest::newHttpRequest();
	}

	req->setMethod(method);
	req->setPath(path);

	for (const auto& [key, value] : queries) {
		req->setParameter(key, value);
	}

	req->addHeader("X-Forwarded-User", authUser); // 認証フィルタを通すためのヘッダ

	std::promise<drogon::HttpResponsePtr> prom;
	auto future = prom.get_future();

	client->sendRequest(req, [&prom](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {
		if (res == drogon::ReqResult::Ok && resp != nullptr) {
			prom.set_value(resp);
		} else {
			prom.set_value(nullptr); // 通信エラー
		}
		});
	return future.get();
}

void uploadDummyFileToMinIO(const std::string& key, const std::string& content) {
	Aws::Client::ClientConfiguration clientConfig;
	clientConfig.region = "us-east-1";
	const char* envEndpoint = std::getenv("MINIO_ENDPOINT");
	clientConfig.endpointOverride = envEndpoint ? std::string("http://") + envEndpoint : "http://minio:9000";
	clientConfig.scheme = Aws::Http::Scheme::HTTP;

	const char* envUser = std::getenv("MINIO_ROOT_USER");
	const char* envPassword = std::getenv("MINIO_ROOT_PASSWORD");
	const std::string accessKey = envUser ? envUser : "";
	const std::string secretKey = envPassword ? envPassword : "";
	Aws::Auth::AWSCredentials credentials(accessKey.c_str(), secretKey.c_str());

	Aws::S3::S3Client s3Client(
		credentials,
		clientConfig,
		Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::RequestDependent,
		false
	);

	Aws::S3::Model::PutObjectRequest request;
	request.SetBucket("videos");
	request.SetKey(key);

	auto inputData = Aws::MakeShared<Aws::StringStream>("PutObjectInputStream");
	*inputData << content;
	request.SetBody(inputData);

	auto outcome = s3Client.PutObject(request);
	if (!outcome.IsSuccess()) {
		std::cerr << "Failed to upload dummy file to MinIO: " << outcome.GetError().GetMessage() << std::endl;
	} else {
		std::cout << "Successfully uploaded dummy file to MinIO with key: " << key << std::endl;
	}
}

std::optional<std::string> postVideo(const std::string& title, const std::string& description = "This is a test video.", const std::string& contentType = "video/mp4", const bool sendWebhook = true) {
	Json::Value createBody;
	createBody["title"] = title;
	createBody["description"] = description;
	createBody["content_type"] = contentType;
	auto createResp = sendSyncRequest(drogon::Post, "/api/videos", createBody);
	if (createResp == nullptr) {
		std::cerr << "Failed to send POST request to create video" << std::endl;
		return std::nullopt;
	}
	if (createResp->getStatusCode() != drogon::k201Created) {
		std::cerr << "Failed to create video, status code: " << createResp->getStatusCode() << std::endl;
		return std::nullopt;
	}
	auto createdJson = createResp->getJsonObject();
	if (createdJson == nullptr || !createdJson->isMember("video_id") || !(*createdJson)["video_id"].isString()) {
		std::cerr << "Invalid response format when creating video" << std::endl;
		return std::nullopt;
	}
	std::string videoId = (*createdJson)["video_id"].asString();

	// Webhookを送信してステータスをCompletedにする
	if (sendWebhook) {
		Json::Value webhookBody;
		webhookBody["video_id"] = videoId;
		webhookBody["status"] = "completed";
		webhookBody["message"] = "success";
		webhookBody["duration"] = 120;
		sendSyncRequest(drogon::Post, "/webhooks/encode_result", webhookBody);
	}
	return videoId;
}

std::optional<std::string> getRedisValueSync(const std::string& key) {
	auto redisClient = drogon::app().getRedisClient();
	std::promise<std::optional<std::string>> prom;
	auto fut = prom.get_future();

	redisClient->execCommandAsync(
		[&prom](const drogon::nosql::RedisResult& r) {
			if (r.isNil()) prom.set_value(std::nullopt);
			else prom.set_value(r.asString());
		},
		[&prom](const std::exception& e) {
			std::cerr << "Redis Error: " << e.what() << std::endl;
			prom.set_value(std::nullopt);
		},
		"GET %s", key.c_str()
	);
	return fut.get();
}

bool deleteVideo(const std::string& videoId, const std::string& authUser = "testuser") {
	Json::Value deleteBody;
	deleteBody["video_id"] = videoId;
	auto deleteResp = sendSyncRequest(drogon::Delete, "/api/videos", deleteBody, {}, authUser);
	return deleteResp != nullptr && deleteResp->getStatusCode() == drogon::k200OK;
}

DROGON_TEST(ApiVideosTest)
{
	// POST,GET,DELETE api/videosのE2Eテスト
	CHECK(postVideo("テスト", "test", "image/png") == std::nullopt);
	std::optional<std::string> videoIdOpt = postVideo("テスト動画");
	REQUIRE(videoIdOpt.has_value());
	std::string videoId = videoIdOpt.value();

	auto getResp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId);
	REQUIRE(getResp != nullptr);
	CHECK(getResp->getStatusCode() == drogon::k200OK);
	auto getJson = getResp->getJsonObject();
	CHECK((*getJson)["title"].asString() == "テスト動画");
	CHECK((*getJson)["description"].asString() == "This is a test video.");

	CHECK(deleteVideo(videoId) == true);

	auto confirmResp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId);
	REQUIRE(confirmResp != nullptr);
	CHECK(confirmResp->getStatusCode() == drogon::k404NotFound);
}

DROGON_TEST(EditVideoTest)
{
	// 動画情報の編集テスト
	std::optional<std::string> videoIdOpt = postVideo("編集前のタイトル", "編集前の説明");
	REQUIRE(videoIdOpt.has_value());
	std::string videoId = videoIdOpt.value();

	Json::Value patchBody;
	patchBody["title"] = "編集後のタイトル";
	patchBody["description"] = "編集後の説明";
	auto patchResp = sendSyncRequest(drogon::Patch, "/api/videos/" + videoId, patchBody);
	REQUIRE(patchResp != nullptr);
	CHECK(patchResp->getStatusCode() == drogon::k200OK);
	auto patchJson = patchResp->getJsonObject();
	int duration = patchJson->get("duration", -1).asInt();
	std::string dateStr = (*patchJson)["created_at"].asString();

	auto getResp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId);
	REQUIRE(getResp != nullptr);
	CHECK(getResp->getStatusCode() == drogon::k200OK);
	auto getJson = getResp->getJsonObject();
	CHECK((*getJson)["title"].asString() == "編集後のタイトル");
	CHECK((*getJson)["description"].asString() == "編集後の説明");
	// 他のパラメータは変更されていないことも確認
	CHECK((*getJson)["duration"].asInt() == duration);
	CHECK((*getJson)["created_at"].asString() == dateStr);

	// 他のユーザーで編集できないことも確認
	patchResp = sendSyncRequest(drogon::Patch, "/api/videos/" + videoId, patchBody, {}, "otheruser");
	REQUIRE(patchResp != nullptr);
	CHECK(patchResp->getStatusCode() == drogon::k403Forbidden);

	// クリーンアップ
	CHECK(deleteVideo(videoId) == true);
}

DROGON_TEST(EditVideoMassAssignmentTest)
{
	std::optional<std::string> videoIdOpt = postVideo("変更されないタイトル", "変更されない説明");
	REQUIRE(videoIdOpt.has_value());
	const std::string videoId = videoIdOpt.value();

	auto beforeResp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId);
	REQUIRE(beforeResp != nullptr);
	REQUIRE(beforeResp->getStatusCode() == drogon::k200OK);
	auto beforeJsonPtr = beforeResp->getJsonObject();
	REQUIRE(beforeJsonPtr != nullptr);
	const Json::Value beforeJson = *beforeJsonPtr;

	const std::vector<std::pair<std::string, Json::Value>> protectedFields = {
		{"video_id", "attacker-video"},
		{"user_id", "otheruser"},
		{"thumbnail_url", "https://example.com/example.png"},
		{"video_url", "https://example.com/watch/attacker-video"},
		{"created_at", "2008-04-23 00:31:07"},
		{"view_count", 999999999},
		{"duration", 8},
		{"like_count", 999999999},
		{"status", 2},
		{"is_external", 1},
		{"type", "youtube"},
	};

	for (const auto& [field, value] : protectedFields) {
		Json::Value body;
		body["title"] = "攻撃者が変更したタイトル";
		body[field] = value;
		auto resp = sendSyncRequest(drogon::Patch, "/api/videos/" + videoId, body);
		REQUIRE(resp != nullptr);
		CHECK(resp->getStatusCode() == drogon::k400BadRequest);
	}

	const std::vector<Json::Value> invalidBodies = {
		Json::Value(Json::objectValue),
		Json::Value(Json::arrayValue),
		Json::Value(42),
	};
	for (const auto& body : invalidBodies) {
		auto resp = sendSyncRequest(drogon::Patch, "/api/videos/" + videoId, body);
		REQUIRE(resp != nullptr);
		CHECK(resp->getStatusCode() == drogon::k400BadRequest);
	}

	Json::Value nonStringTitle;
	nonStringTitle["title"] = 42;
	auto invalidTypeResp = sendSyncRequest(drogon::Patch, "/api/videos/" + videoId, nonStringTitle);
	REQUIRE(invalidTypeResp != nullptr);
	CHECK(invalidTypeResp->getStatusCode() == drogon::k400BadRequest);

	auto afterResp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId);
	REQUIRE(afterResp != nullptr);
	REQUIRE(afterResp->getStatusCode() == drogon::k200OK);
	auto afterJson = afterResp->getJsonObject();
	REQUIRE(afterJson != nullptr);
	CHECK(*afterJson == beforeJson);

	CHECK(deleteVideo(videoId) == true);
}

DROGON_TEST(ExternalVideoSyncTest)
{
	auto dbClient = drogon::app().getDbClient();
	const std::string videoId = "external-sync-test";
	const std::string secondVideoId = "external-sync-test-2";
	const std::string partialVideoId = "external-sync-partial";
	const std::string failedVideoId = "external-sync-fail";
	const std::string missingUpstreamVideoId = "external-sync-missing";
	const std::string internalVideoId = "internal-sync-test";
	dbClient->execSqlSync(
		"DELETE FROM videos WHERE video_id IN (?, ?, ?, ?, ?, ?)",
		videoId,
		secondVideoId,
		partialVideoId,
		failedVideoId,
		missingUpstreamVideoId,
		internalVideoId
	);
	dbClient->execSqlSync(
		"INSERT INTO videos "
		"(video_id, user_id, title, description, video_url, view_count, duration, "
		"like_count, status, is_external, type) "
		"VALUES (?, 'testuser', '同期前タイトル', '同期前説明', 'MOCKVIDEO01', 7, 9, 8, 2, 1, 'youtube')",
		videoId
	);
	dbClient->execSqlSync(
		"INSERT INTO videos "
		"(video_id, user_id, title, description, video_url, view_count, duration, "
		"like_count, status, is_external, type) "
		"VALUES (?, 'testuser', '同期前タイトル2', '同期前説明2', 'MOCKVIDEO02', 17, 19, 18, 2, 1, 'youtube')",
		secondVideoId
	);
	dbClient->execSqlSync(
		"INSERT INTO videos "
		"(video_id, user_id, title, description, video_url, view_count, duration, "
		"like_count, status, is_external, type) "
		"VALUES (?, 'testuser', '部分更新前タイトル', '部分更新前説明', 'PARTVIDEO01', 27, 29, 28, 2, 1, 'youtube')",
		partialVideoId
	);
	dbClient->execSqlSync(
		"INSERT INTO videos "
		"(video_id, user_id, title, description, video_url, status, is_external, type) "
		"VALUES (?, 'testuser', '失敗前タイトル', '失敗前説明', 'FAILVIDEO01', 2, 1, 'youtube')",
		failedVideoId
	);
	dbClient->execSqlSync(
		"INSERT INTO videos "
		"(video_id, user_id, title, description, video_url, status, is_external, type) "
		"VALUES (?, 'testuser', '欠落前タイトル', '欠落前説明', 'MISSVIDEO01', 2, 1, 'youtube')",
		missingUpstreamVideoId
	);
	dbClient->execSqlSync(
		"INSERT INTO videos "
		"(video_id, user_id, title, description, video_url, status, is_external, type) "
		"VALUES (?, 'testuser', '内部動画', '内部動画説明', '/watch/internal', 2, 0, 'internal')",
		internalVideoId
	);

	Json::Value syncBody;
	syncBody["ids"] = Json::Value(Json::arrayValue);
	syncBody["ids"].append(videoId);
	syncBody["ids"].append(secondVideoId);
	syncBody["targets"]["video"]["fields"] = Json::Value(Json::arrayValue);
	syncBody["targets"]["video"]["fields"].append("title");
	syncBody["targets"]["statistics"]["fields"] = Json::Value(Json::arrayValue);
	syncBody["targets"]["statistics"]["fields"].append("like_count");
	syncBody["targets"]["statistics"]["fields"].append("comment_count");
	g_youtubeVideosListCalls.store(0, std::memory_order_relaxed);

	auto syncResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/sync",
		syncBody
	);
	REQUIRE(syncResponse != nullptr);
	CHECK(syncResponse->getStatusCode() == drogon::k200OK);
	auto syncJson = syncResponse->getJsonObject();
	REQUIRE(syncJson != nullptr);
	REQUIRE((*syncJson)["results"].isArray());
	REQUIRE((*syncJson)["results"].size() == 2);
	const Json::Value& firstResult = (*syncJson)["results"][0];
	const Json::Value& secondResult = (*syncJson)["results"][1];
	CHECK(firstResult["id"].asString() == videoId);
	CHECK(firstResult["status"].asString() == "updated");
	CHECK(firstResult["video"]["title"].asString() == "YouTube同期タイトル");
	CHECK(firstResult["video"]["description"].asString() == "同期前説明");
	CHECK(firstResult["video"]["duration"].asInt() == 9);
	CHECK(secondResult["id"].asString() == secondVideoId);
	CHECK(secondResult["status"].asString() == "updated");
	CHECK(secondResult["video"]["title"].asString() == "YouTube同期タイトル2");
	CHECK(secondResult["video"]["description"].asString() == "同期前説明2");
	CHECK(secondResult["video"]["duration"].asInt() == 19);
	CHECK((*syncJson)["quota_units"].asInt() == 1);
	CHECK(firstResult["synced"]["statistics"]["like_count"].asUInt64() == 56);
	CHECK(firstResult["synced"]["statistics"]["comment_count"].asUInt64() == 78);
	CHECK(secondResult["synced"]["statistics"]["like_count"].asUInt64() == 66);
	CHECK(secondResult["synced"]["statistics"]["comment_count"].asUInt64() == 88);
	CHECK(!firstResult["synced"]["statistics"].isMember("view_count"));
	CHECK(!firstResult["synced"].isMember("comments"));
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 1);
	{
		std::lock_guard<std::mutex> lock(g_youtubeRequestMutex);
		CHECK(g_lastYoutubeVideoParts ==
			"id,snippet,contentDetails,status,statistics,paidProductPlacementDetails,"
			"player,topicDetails,recordingDetails,liveStreamingDetails,brandPartner,localizations");
		CHECK(g_lastYoutubeVideoParts.find("fileDetails") == std::string::npos);
		CHECK(g_lastYoutubeVideoParts.find("processingDetails") == std::string::npos);
		CHECK(g_lastYoutubeVideoParts.find("suggestions") == std::string::npos);
		CHECK(g_lastYoutubeVideoIds == "MOCKVIDEO01,MOCKVIDEO02");
	}
	const Json::Value& publicMetadata = firstResult["metadata"];
	CHECK(publicMetadata["snippet"]["channelTitle"].asString() == "公開チャンネル");
	CHECK(publicMetadata["contentDetails"]["definition"].asString() == "hd");
	CHECK(publicMetadata["status"]["privacyStatus"].asString() == "public");
	CHECK(publicMetadata["statistics"]["commentCount"].asString() == "78");
	CHECK(publicMetadata["paidProductPlacementDetails"].isObject());
	CHECK(publicMetadata["player"].isObject());
	CHECK(publicMetadata["topicDetails"].isObject());
	CHECK(publicMetadata["recordingDetails"].isObject());
	CHECK(publicMetadata["liveStreamingDetails"].isObject());
	CHECK(publicMetadata["brandPartner"].isObject());
	CHECK(publicMetadata["localizations"].isObject());
	CHECK(!publicMetadata.isMember("fileDetails"));
	CHECK(!publicMetadata.isMember("processingDetails"));
	CHECK(!publicMetadata.isMember("suggestions"));
	CHECK(!publicMetadata["contentDetails"].isMember("hasCustomThumbnail"));
	CHECK(!publicMetadata["status"].isMember("selfDeclaredMadeForKids"));
	CHECK(!publicMetadata["statistics"].isMember("dislikeCount"));

	auto videoRows = dbClient->execSqlSync(
		"SELECT title, description, duration, view_count, like_count FROM videos WHERE video_id = ?",
		videoId
	);
	REQUIRE(videoRows.size() == 1);
	CHECK(videoRows[0]["title"].as<std::string>() == "YouTube同期タイトル");
	CHECK(videoRows[0]["description"].as<std::string>() == "同期前説明");
	CHECK(videoRows[0]["duration"].as<int>() == 9);
	CHECK(videoRows[0]["view_count"].as<int>() == 7);
	CHECK(videoRows[0]["like_count"].as<int>() == 8);

	auto statisticRows = dbClient->execSqlSync(
		"SELECT view_count, like_count, comment_count FROM external_video_statistics WHERE video_id = ?",
		videoId
	);
	REQUIRE(statisticRows.size() == 1);
	CHECK(statisticRows[0]["view_count"].isNull());
	CHECK(statisticRows[0]["like_count"].as<uint64_t>() == 56);
	CHECK(statisticRows[0]["comment_count"].as<uint64_t>() == 78);
	auto secondStatisticRows = dbClient->execSqlSync(
		"SELECT view_count, like_count, comment_count FROM external_video_statistics WHERE video_id = ?",
		secondVideoId
	);
	REQUIRE(secondStatisticRows.size() == 1);
	CHECK(secondStatisticRows[0]["view_count"].isNull());
	CHECK(secondStatisticRows[0]["like_count"].as<uint64_t>() == 66);
	CHECK(secondStatisticRows[0]["comment_count"].as<uint64_t>() == 88);

	auto metadataRows = dbClient->execSqlSync(
		"SELECT metadata FROM external_video_metadata WHERE video_id = ?",
		videoId
	);
	REQUIRE(metadataRows.size() == 1);
	Json::Value savedMetadata;
	Json::Reader metadataReader;
	REQUIRE(metadataReader.parse(metadataRows[0]["metadata"].as<std::string>(), savedMetadata));
	CHECK(savedMetadata == publicMetadata);

	Json::Value secondSyncBody;
	secondSyncBody["ids"] = Json::Value(Json::arrayValue);
	secondSyncBody["ids"].append(videoId);
	secondSyncBody["targets"]["video"]["fields"] = Json::Value(Json::arrayValue);
	secondSyncBody["targets"]["video"]["fields"].append("description");
	secondSyncBody["targets"]["video"]["fields"].append("duration");
	secondSyncBody["targets"]["video"]["fields"].append("type");
	secondSyncBody["targets"]["statistics"]["fields"] = Json::Value(Json::arrayValue);
	secondSyncBody["targets"]["statistics"]["fields"].append("view_count");
	auto secondSyncResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/sync",
		secondSyncBody
	);
	REQUIRE(secondSyncResponse != nullptr);
	CHECK(secondSyncResponse->getStatusCode() == drogon::k200OK);
	auto secondSyncJson = secondSyncResponse->getJsonObject();
	REQUIRE(secondSyncJson != nullptr);
	REQUIRE((*secondSyncJson)["results"].size() == 1);
	const Json::Value& secondSyncResult = (*secondSyncJson)["results"][0];
	CHECK(secondSyncResult["id"].asString() == videoId);
	CHECK(secondSyncResult["status"].asString() == "updated");
	CHECK(secondSyncResult["video"]["title"].asString() == "YouTube同期タイトル");
	CHECK(secondSyncResult["video"]["description"].asString() == "YouTube同期説明");
	CHECK(secondSyncResult["video"]["duration"].asInt() == 123);
	CHECK(secondSyncResult["video"]["type"].asString() == "youtube");
	CHECK((*secondSyncJson)["quota_units"].asInt() == 1);
	CHECK(secondSyncResult["synced"]["statistics"]["view_count"].asUInt64() == 1234);
	CHECK(!secondSyncResult["synced"]["statistics"].isMember("like_count"));
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 2);

	statisticRows = dbClient->execSqlSync(
		"SELECT view_count, like_count, comment_count FROM external_video_statistics WHERE video_id = ?",
		videoId
	);
	REQUIRE(statisticRows.size() == 1);
	CHECK(statisticRows[0]["view_count"].as<uint64_t>() == 1234);
	CHECK(statisticRows[0]["like_count"].as<uint64_t>() == 56);
	CHECK(statisticRows[0]["comment_count"].as<uint64_t>() == 78);

	Json::Value partialSyncBody;
	partialSyncBody["ids"] = Json::Value(Json::arrayValue);
	partialSyncBody["ids"].append(partialVideoId);
	partialSyncBody["targets"]["video"]["fields"] = Json::Value(Json::arrayValue);
	partialSyncBody["targets"]["video"]["fields"].append("title");
	partialSyncBody["targets"]["video"]["fields"].append("description");
	partialSyncBody["targets"]["video"]["fields"].append("duration");
	partialSyncBody["targets"]["video"]["fields"].append("type");
	partialSyncBody["targets"]["statistics"]["fields"] = Json::Value(Json::arrayValue);
	partialSyncBody["targets"]["statistics"]["fields"].append("view_count");
	partialSyncBody["targets"]["statistics"]["fields"].append("like_count");
	partialSyncBody["targets"]["statistics"]["fields"].append("comment_count");
	auto partialSyncResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/sync",
		partialSyncBody
	);
	REQUIRE(partialSyncResponse != nullptr);
	CHECK(partialSyncResponse->getStatusCode() == drogon::k200OK);
	auto partialSyncJson = partialSyncResponse->getJsonObject();
	REQUIRE(partialSyncJson != nullptr);
	REQUIRE((*partialSyncJson)["results"].size() == 1);
	const Json::Value& partialResult = (*partialSyncJson)["results"][0];
	CHECK(partialResult["id"].asString() == partialVideoId);
	CHECK(partialResult["status"].asString() == "partial");
	CHECK(partialResult["video"]["title"].asString() == "YouTube同期タイトル");
	CHECK(partialResult["video"]["description"].asString() == "部分更新前説明");
	CHECK(partialResult["video"]["duration"].asInt() == 123);
	CHECK(partialResult["synced"]["video"]["fields"].size() == 3);
	CHECK(partialResult["skipped"]["video"]["description"].asString() == "not_returned");
	CHECK(partialResult["synced"]["statistics"]["view_count"].asUInt64() == 1234);
	CHECK(partialResult["synced"]["statistics"]["comment_count"].asUInt64() == 78);
	CHECK(partialResult["skipped"]["statistics"]["like_count"].asString() == "not_returned");
	CHECK((*partialSyncJson)["quota_units"].asInt() == 1);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 3);

	auto partialRows = dbClient->execSqlSync(
		"SELECT title, description, duration FROM videos WHERE video_id = ?",
		partialVideoId
	);
	REQUIRE(partialRows.size() == 1);
	CHECK(partialRows[0]["title"].as<std::string>() == "YouTube同期タイトル");
	CHECK(partialRows[0]["description"].as<std::string>() == "部分更新前説明");
	CHECK(partialRows[0]["duration"].as<int>() == 123);
	auto partialStatisticRows = dbClient->execSqlSync(
		"SELECT view_count, like_count, comment_count FROM external_video_statistics WHERE video_id = ?",
		partialVideoId
	);
	REQUIRE(partialStatisticRows.size() == 1);
	CHECK(partialStatisticRows[0]["view_count"].as<uint64_t>() == 1234);
	CHECK(partialStatisticRows[0]["like_count"].isNull());
	CHECK(partialStatisticRows[0]["comment_count"].as<uint64_t>() == 78);

	auto forbiddenResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/sync",
		syncBody,
		{},
		"otheruser"
	);
	REQUIRE(forbiddenResponse != nullptr);
	CHECK(forbiddenResponse->getStatusCode() == drogon::k403Forbidden);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 3);

	Json::Value invalidBody;
	invalidBody["ids"] = Json::Value(Json::arrayValue);
	invalidBody["ids"].append(videoId);
	invalidBody["targets"]["video"]["fields"] = Json::Value(Json::arrayValue);
	invalidBody["targets"]["video"]["fields"].append("video_url");
	auto invalidResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/sync",
		invalidBody
	);
	REQUIRE(invalidResponse != nullptr);
	CHECK(invalidResponse->getStatusCode() == drogon::k400BadRequest);

	const std::vector<Json::Value> malformedBodies = {
		Json::Value(Json::objectValue),
		[&] {
			Json::Value body;
			body["ids"] = Json::Value(Json::arrayValue);
			body["ids"].append(videoId);
			body["targets"] = Json::Value(Json::objectValue);
			return body;
		}(),
		[&] {
			Json::Value body;
			body["ids"] = Json::Value(Json::arrayValue);
			body["ids"].append(videoId);
			body["targets"]["comments"]["enabled"] = true;
			return body;
		}(),
		[&] {
			Json::Value body;
			body["ids"] = Json::Value(Json::arrayValue);
			body["ids"].append(videoId);
			body["targets"]["video"]["fields"] = Json::Value(Json::arrayValue);
			body["targets"]["video"]["fields"].append("title");
			body["targets"]["video"]["fields"].append("title");
			return body;
		}(),
		[&] {
			Json::Value body = secondSyncBody;
			body["ids"].append(videoId);
			return body;
		}(),
		[&] {
			Json::Value body = secondSyncBody;
			body["unexpected"] = true;
			return body;
		}(),
		[&] {
			Json::Value body = secondSyncBody;
			body["ids"] = Json::Value(Json::arrayValue);
			for (int i = 0; i < 51; ++i) {
				body["ids"].append("video-" + std::to_string(i));
			}
			return body;
		}(),
	};
	for (const auto& malformedBody : malformedBodies) {
		auto malformedResponse = sendSyncRequest(
			drogon::Post,
			"/api/ex-videos/sync",
			malformedBody
		);
		REQUIRE(malformedResponse != nullptr);
		CHECK(malformedResponse->getStatusCode() == drogon::k400BadRequest);
	}
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 3);

	Json::Value internalBody = syncBody;
	internalBody["ids"] = Json::Value(Json::arrayValue);
	internalBody["ids"].append(videoId);
	internalBody["ids"].append(internalVideoId);
	auto internalResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/sync",
		internalBody
	);
	REQUIRE(internalResponse != nullptr);
	CHECK(internalResponse->getStatusCode() == drogon::k409Conflict);
	Json::Value missingBody = syncBody;
	missingBody["ids"] = Json::Value(Json::arrayValue);
	missingBody["ids"].append(videoId);
	missingBody["ids"].append("missing-video");
	auto missingResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/sync",
		missingBody
	);
	REQUIRE(missingResponse != nullptr);
	CHECK(missingResponse->getStatusCode() == drogon::k404NotFound);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 3);

	Json::Value failureBody;
	failureBody["ids"] = Json::Value(Json::arrayValue);
	failureBody["ids"].append(videoId);
	failureBody["ids"].append(failedVideoId);
	failureBody["targets"]["video"]["fields"] = Json::Value(Json::arrayValue);
	failureBody["targets"]["video"]["fields"].append("title");
	dbClient->execSqlSync(
		"UPDATE videos SET title = '原子性確認タイトル' WHERE video_id = ?",
		videoId
	);
	auto failureResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/sync",
		failureBody
	);
	REQUIRE(failureResponse != nullptr);
	CHECK(failureResponse->getStatusCode() == drogon::k502BadGateway);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 4);
	auto failedRows = dbClient->execSqlSync(
		"SELECT title FROM videos WHERE video_id = ?",
		failedVideoId
	);
	REQUIRE(failedRows.size() == 1);
	CHECK(failedRows[0]["title"].as<std::string>() == "失敗前タイトル");
	auto atomicRows = dbClient->execSqlSync(
		"SELECT title FROM videos WHERE video_id = ?",
		videoId
	);
	REQUIRE(atomicRows.size() == 1);
	CHECK(atomicRows[0]["title"].as<std::string>() == "原子性確認タイトル");

	Json::Value missingUpstreamBody = failureBody;
	missingUpstreamBody["ids"] = Json::Value(Json::arrayValue);
	missingUpstreamBody["ids"].append(videoId);
	missingUpstreamBody["ids"].append(missingUpstreamVideoId);
	auto missingUpstreamResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/sync",
		missingUpstreamBody
	);
	REQUIRE(missingUpstreamResponse != nullptr);
	CHECK(missingUpstreamResponse->getStatusCode() == drogon::k200OK);
	auto missingUpstreamJson = missingUpstreamResponse->getJsonObject();
	REQUIRE(missingUpstreamJson != nullptr);
	REQUIRE((*missingUpstreamJson)["results"].size() == 2);
	CHECK((*missingUpstreamJson)["quota_units"].asInt() == 1);
	CHECK((*missingUpstreamJson)["results"][0]["id"].asString() == videoId);
	CHECK((*missingUpstreamJson)["results"][0]["status"].asString() == "updated");
	CHECK((*missingUpstreamJson)["results"][1]["id"].asString() == missingUpstreamVideoId);
	CHECK((*missingUpstreamJson)["results"][1]["status"].asString() == "skipped");
	CHECK((*missingUpstreamJson)["results"][1]["reason"].asString() == "youtube_video_not_returned");
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 5);
	atomicRows = dbClient->execSqlSync(
		"SELECT title FROM videos WHERE video_id = ?",
		videoId
	);
	REQUIRE(atomicRows.size() == 1);
	CHECK(atomicRows[0]["title"].as<std::string>() == "YouTube同期タイトル");
	auto missingUpstreamRows = dbClient->execSqlSync(
		"SELECT title FROM videos WHERE video_id = ?",
		missingUpstreamVideoId
	);
	REQUIRE(missingUpstreamRows.size() == 1);
	CHECK(missingUpstreamRows[0]["title"].as<std::string>() == "欠落前タイトル");
	auto missingMetadataRows = dbClient->execSqlSync(
		"SELECT video_id FROM external_video_metadata WHERE video_id = ?",
		missingUpstreamVideoId
	);
	CHECK(missingMetadataRows.empty());

	dbClient->execSqlSync(
		"DELETE FROM videos WHERE video_id IN (?, ?, ?, ?, ?, ?)",
		videoId,
		secondVideoId,
		partialVideoId,
		failedVideoId,
		missingUpstreamVideoId,
		internalVideoId
	);
}

DROGON_TEST(SearchTest)
{
	// 動画を投稿
	std::optional<std::string> videoId1Opt = postVideo("1猫の動画", "にゃーん");
	std::optional<std::string> videoId2Opt = postVideo("2犬の動画", "きゃんきゃん");
	std::optional<std::string> videoId3Opt = postVideo("3ヌオーの動画", "ヌオー");
	std::optional<std::string> videoId4Opt = postVideo("4空白の動画", "empty");
	std::optional<std::string> videoId5Opt = postVideo("5帝国", "empire");

	if (!videoId1Opt.has_value() || !videoId2Opt.has_value() || !videoId3Opt.has_value() || !videoId4Opt.has_value() || !videoId5Opt.has_value()) {
		REQUIRE(false);
	}

	std::string videoId1 = videoId1Opt.value();
	std::string videoId2 = videoId2Opt.value();
	std::string videoId3 = videoId3Opt.value();
	std::string videoId4 = videoId4Opt.value();
	std::string videoId5 = videoId5Opt.value();
	// 投稿された動画の中から検索
	std::unordered_map<std::string, std::string> queries = {
		{"search", "動画"},
		{"sortby", "title"},
		{"order", "1"}
	};
	auto searchResp = sendSyncRequest(drogon::Get, "/api/videos", Json::Value::null, queries);
	REQUIRE(searchResp != nullptr);
	CHECK(searchResp->getStatusCode() == drogon::k200OK);
	auto searchJson = searchResp->getJsonObject();
	REQUIRE(searchJson != nullptr);
	CHECK(searchJson->isArray());
	CHECK(searchJson->size() == 4);
	CHECK((*searchJson)[0]["video_id"].asString() == videoId1);
	CHECK((*searchJson)[1]["video_id"].asString() == videoId2);
	CHECK((*searchJson)[2]["video_id"].asString() == videoId3);
	CHECK((*searchJson)[3]["video_id"].asString() == videoId4);
	// 検索その2
	queries = {
		{"search", "em"},
		{"sortby", "title"},
		{"order", "0"}
	};
	searchResp = sendSyncRequest(drogon::Get, "/api/videos", Json::Value::null, queries);
	REQUIRE(searchResp != nullptr);
	CHECK(searchResp->getStatusCode() == drogon::k200OK);
	searchJson = searchResp->getJsonObject();
	REQUIRE(searchJson != nullptr);
	CHECK(searchJson->isArray());
	CHECK(searchJson->size() == 2);
	CHECK((*searchJson)[0]["video_id"].asString() == videoId5);
	CHECK((*searchJson)[1]["video_id"].asString() == videoId4);
	// タグ付与
	Json::Value tagBody;
	tagBody["tag"] = "かわいい";
	auto tagResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId1 + "/tags", tagBody);
	REQUIRE(tagResp != nullptr);
	CHECK(tagResp->getStatusCode() == drogon::k200OK);
	tagResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId2 + "/tags", tagBody);
	REQUIRE(tagResp != nullptr);
	CHECK(tagResp->getStatusCode() == drogon::k200OK);
	tagResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId3 + "/tags", tagBody);
	REQUIRE(tagResp != nullptr);
	CHECK(tagResp->getStatusCode() == drogon::k200OK);
	// タグ検索
	queries = {
		{"search", "ー"},
		{"tag", "かわいい"},
		{"sortby", "title"},
		{"order", "1"}
	};
	searchResp = sendSyncRequest(drogon::Get, "/api/videos", Json::Value::null, queries);
	REQUIRE(searchResp != nullptr);
	CHECK(searchResp->getStatusCode() == drogon::k200OK);
	searchJson = searchResp->getJsonObject();
	REQUIRE(searchJson != nullptr);
	CHECK(searchJson->isArray());
	CHECK(searchJson->size() == 2);
	CHECK((*searchJson)[0]["video_id"].asString() == videoId1);
	CHECK((*searchJson)[1]["video_id"].asString() == videoId3);
	// タグ複数付与テスト
	tagBody["tag"] = "人類には早すぎる動画";
	tagResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId3 + "/tags", tagBody);
	REQUIRE(tagResp != nullptr);
	CHECK(tagResp->getStatusCode() == drogon::k200OK);
	tagResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId4 + "/tags", tagBody);
	REQUIRE(tagResp != nullptr);
	CHECK(tagResp->getStatusCode() == drogon::k200OK);
	// タグ検索2
	queries = {
		{"search", ""},
		{"tag", "人類には早すぎる動画"},
		{"sortby", "title"},
		{"order", "1"}
	};
	searchResp = sendSyncRequest(drogon::Get, "/api/videos", Json::Value::null, queries);
	REQUIRE(searchResp != nullptr);
	CHECK(searchResp->getStatusCode() == drogon::k200OK);
	searchJson = searchResp->getJsonObject();
	REQUIRE(searchJson != nullptr);
	CHECK(searchJson->isArray());
	CHECK(searchJson->size() == 2);
	CHECK((*searchJson)[0]["video_id"].asString() == videoId3);
	CHECK((*searchJson)[1]["video_id"].asString() == videoId4);
	// タグ情報取得
	auto tagInfo = sendSyncRequest(drogon::Get, "/api/videos/" + videoId4 + "/tags");
	REQUIRE(tagInfo != nullptr);
	CHECK(tagInfo->getStatusCode() == drogon::k200OK);
	auto tagInfoJson = tagInfo->getJsonObject();
	REQUIRE(tagInfoJson != nullptr);
	CHECK(tagInfoJson->isArray());
	CHECK(tagInfoJson->size() == 1);
	CHECK((*tagInfoJson)[0]["name"].asString() == "人類には早すぎる動画");
	// タグ削除
	Json::Value removeTagBody;
	removeTagBody["tag_id"] = (*tagInfoJson)[0]["tag_id"].asInt();
	auto removeTagResp = sendSyncRequest(drogon::Delete, "/api/videos/" + videoId4 + "/tags", removeTagBody);
	REQUIRE(removeTagResp != nullptr);
	CHECK(removeTagResp->getStatusCode() == drogon::k200OK);
	// タグ削除確認
	auto confirmTagInfo = sendSyncRequest(drogon::Get, "/api/videos/" + videoId4 + "/tags");
	REQUIRE(confirmTagInfo != nullptr);
	CHECK(confirmTagInfo->getStatusCode() == drogon::k200OK);
	auto confirmTagInfoJson = confirmTagInfo->getJsonObject();
	REQUIRE(confirmTagInfoJson != nullptr);
	CHECK(confirmTagInfoJson->isArray());
	CHECK(confirmTagInfoJson->size() == 0);
	// クリーンアップ
	CHECK(deleteVideo(videoId1) == true);
	CHECK(deleteVideo(videoId2) == true);
	CHECK(deleteVideo(videoId3) == true);
	CHECK(deleteVideo(videoId4) == true);
	CHECK(deleteVideo(videoId5) == true);
	// タグのクリーンアップ
	std::unordered_map<std::string, std::string> CleanupQueries = {
	{"query", "%"},
	};
	auto allTagsResp = sendSyncRequest(drogon::Get, "/api/tag", Json::Value::null, CleanupQueries);
	REQUIRE(allTagsResp != nullptr);
	CHECK(allTagsResp->getStatusCode() == drogon::k200OK);
	auto allTagsJson = allTagsResp->getJsonObject();
	REQUIRE(allTagsJson != nullptr);
	CHECK(allTagsJson->isArray());
	for (size_t i = 0; i < allTagsJson->size(); ++i) {
		int tagId = (*allTagsJson)[static_cast<int>(i)]["tag_id"].asInt();
		Json::Value deleteTagBody;
		deleteTagBody["tag_id"] = tagId;
		auto deleteTagResp = sendSyncRequest(drogon::Delete, "/api/tag", deleteTagBody);
		REQUIRE(deleteTagResp != nullptr);
		CHECK(deleteTagResp->getStatusCode() == drogon::k200OK);
	}
}

DROGON_TEST(WebhookTest)
{
	// WebhookのE2Eテスト
	std::optional<std::string> videoIdOpt = postVideo("Webhookと再生のテスト");
	REQUIRE(videoIdOpt.has_value());
	std::string videoId = videoIdOpt.value();

	Json::Value webhookBody;
	webhookBody["video_id"] = videoId;
	webhookBody["status"] = "completed";
	webhookBody["message"] = "success";
	webhookBody["duration"] = 120;
	auto webhookResp = sendSyncRequest(drogon::Post, "/webhooks/encode_result", webhookBody);
	REQUIRE(webhookResp != nullptr);
	CHECK(webhookResp->getStatusCode() == drogon::k200OK);

	std::string dummyM3u8 =
		"#EXTM3U\n"
		"#EXT-X-VERSION:3\n"
		"#EXT-X-MAP:URI=\"init.mp4\"\n"
		"segment0.ts\n";
	uploadDummyFileToMinIO("hls/" + videoId + "/output.m3u8", dummyM3u8);
	auto playResp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId + "/play");
	REQUIRE(playResp != nullptr);
	CHECK(playResp->getStatusCode() == drogon::k200OK);

	std::string responseM3u8 = std::string(playResp->getBody());
	CHECK(responseM3u8.find("X-Amz-Signature=") != std::string::npos);
	CHECK(responseM3u8.find("X-Amz-Credential=") != std::string::npos);
	CHECK(responseM3u8.find("X-Amz-Expires=") != std::string::npos);
	CHECK(responseM3u8.find("segment0.ts") != std::string::npos);
	// クリーンアップ
	CHECK(deleteVideo(videoId) == true);
}

DROGON_TEST(ProgressTest)
{
	std::string videoId = drogon::utils::genRandomString(11);
	auto dbClient = drogon::app().getDbClient();
	try {
		dbClient->execSqlSync(
			"INSERT INTO videos (video_id, user_id,video_url, title, status) "
			"VALUES (?, 'test_user', 'https://example.com/video.mp4', '進捗テスト', 1)",
			videoId
		);
	}
	catch (const drogon::orm::DrogonDbException& e) {
		std::cerr << "DB Error: " << e.base().what() << std::endl;
	}

	auto redisClient = drogon::app().getRedisClient();
	REQUIRE(redisClient != nullptr);
	std::promise<void> redisProm;
	auto redisFut = redisProm.get_future();
	std::string redisKey = "video:progress:" + videoId;
	redisClient->execCommandAsync(
		[&redisProm](const drogon::nosql::RedisResult& r) {
			std::cerr << "SET '75%' completed" << std::endl;
			redisProm.set_value();
		},
		[&redisProm](const std::exception& e) {
			std::cerr << "Error setting Redis value: " << e.what() << std::endl;
			redisProm.set_value();
		},
		"SET %s %d", redisKey.c_str(), 75
	);
	redisFut.get();

	auto getResp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId + "/progress");
	REQUIRE(getResp != nullptr);
	CHECK(getResp->getStatusCode() == drogon::k200OK);
	auto getJson = getResp->getJsonObject();
	REQUIRE(getJson != nullptr);
	CHECK((*getJson)["progress"].asInt() == 75);
	CHECK((*getJson)["status"].asInt() == 1);
	// クリーンアップ
	try {
		dbClient->execSqlSync("DELETE FROM videos WHERE video_id = ?", videoId);
	}
	catch (const drogon::orm::DrogonDbException& e) {
		std::cerr << "DB Cleanup Error: " << e.base().what() << std::endl;
	}
}

DROGON_TEST(CommentTest)
{
	std::optional<std::string> videoIdOpt = postVideo("コメントテスト");
	REQUIRE(videoIdOpt.has_value());
	std::string videoId = videoIdOpt.value();
	Json::Value commentBody;
	commentBody["content"] = "テストコメント";
	commentBody["timestamp"] = 10.02;
	commentBody["command"] = "red ue";
	auto commentResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId + "/comments", commentBody);
	REQUIRE(commentResp != nullptr);
	CHECK(commentResp->getStatusCode() == drogon::k201Created);
	auto commentJson = commentResp->getJsonObject();
	REQUIRE(commentJson != nullptr);
	CHECK((*commentJson)["comment"].asString() == "テストコメント");
	CHECK((*commentJson)["timestamp"].asDouble() == 10.02);
	CHECK((*commentJson)["command"].asString() == "red ue");
	// コメント追加
	commentBody["content"] = "2つ目のコメント";
	commentBody["timestamp"] = 20.05;
	commentBody["command"] = "blue shita";
	commentResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId + "/comments", commentBody);
	REQUIRE(commentResp != nullptr);
	CHECK(commentResp->getStatusCode() == drogon::k201Created);
	// コメント取得
	auto getCommentsResp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId + "/comments");
	REQUIRE(getCommentsResp != nullptr);
	CHECK(getCommentsResp->getStatusCode() == drogon::k200OK);
	auto getCommentsJson = getCommentsResp->getJsonObject();
	REQUIRE(getCommentsJson != nullptr);
	CHECK(getCommentsJson->isArray());
	CHECK(getCommentsJson->size() == 2);
	CHECK((*getCommentsJson)[0]["comment"].asString() == "テストコメント");
	CHECK((*getCommentsJson)[0]["timestamp"].asDouble() == 10.02);
	CHECK((*getCommentsJson)[0]["command"].asString() == "red ue");
	CHECK((*getCommentsJson)[1]["comment"].asString() == "2つ目のコメント");
	CHECK((*getCommentsJson)[1]["timestamp"].asDouble() == 20.05);
	CHECK((*getCommentsJson)[1]["command"].asString() == "blue shita");
	// コメント削除
	Json::Value deleteCommentBody;
	deleteCommentBody["comment_id"] = (*getCommentsJson)[0]["comment_id"].asInt();
	auto deleteCommentResp = sendSyncRequest(drogon::Delete, "/api/videos/" + videoId + "/comments", deleteCommentBody);
	REQUIRE(deleteCommentResp != nullptr);
	CHECK(deleteCommentResp->getStatusCode() == drogon::k200OK);
	// コメント削除確認
	getCommentsResp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId + "/comments");
	REQUIRE(getCommentsResp != nullptr);
	CHECK(getCommentsResp->getStatusCode() == drogon::k200OK);
	getCommentsJson = getCommentsResp->getJsonObject();
	REQUIRE(getCommentsJson != nullptr);
	CHECK(getCommentsJson->isArray());
	CHECK(getCommentsJson->size() == 1);
	CHECK((*getCommentsJson)[0]["comment"].asString() == "2つ目のコメント");
	CHECK((*getCommentsJson)[0]["timestamp"].asDouble() == 20.05);
	CHECK((*getCommentsJson)[0]["command"].asString() == "blue shita");
	// クリーンアップ
	CHECK(deleteVideo(videoId) == true);
}

DROGON_TEST(ViewCountIncTest)
{
	std::optional<std::string> videoIdOpt = postVideo("再生回数増加テスト");
	REQUIRE(videoIdOpt.has_value());
	std::string videoId = videoIdOpt.value();

	auto playResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId + "/views");
	REQUIRE(playResp != nullptr);
	CHECK(playResp->getStatusCode() == drogon::k200OK);
	auto json1 = playResp->getJsonObject();
	REQUIRE(json1 != nullptr);
	CHECK((*json1)["counted"].asBool() == true);
	// 再生回数が1増えていることを確認
	auto redisVal1 = getRedisValueSync("pending_views:" + videoId);
	REQUIRE(redisVal1.has_value());
	CHECK(redisVal1.value() == "1");
	// 短時間で複数回再生しても再生回数が1しか増えないことを確認
	auto resp2 = sendSyncRequest(drogon::Post, "/api/videos/" + videoId + "/views");
	REQUIRE(resp2 != nullptr);
	CHECK(resp2->getStatusCode() == drogon::k200OK);
	auto json2 = resp2->getJsonObject();
	REQUIRE(json2 != nullptr);
	CHECK((*json2)["counted"].asBool() == false);
	// 再生回数が増えていないことを確認
	auto redisVal2 = getRedisValueSync("pending_views:" + videoId);
	REQUIRE(redisVal2.has_value());
	CHECK(redisVal2.value() == "1");
	// クリーンアップ
	CHECK(deleteVideo(videoId) == true);
}

DROGON_TEST(ThumbnailTest)
{
	std::optional<std::string> videoIdOpt = postVideo("サムネイルテスト");
	REQUIRE(videoIdOpt.has_value());
	std::string videoId = videoIdOpt.value();
	std::string thumbnailId = "thumbnail.jpg";
	// vttファイルから呼び出されるAPIのテスト
	auto resp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId + "/thumbnails/" + thumbnailId);
	REQUIRE(resp != nullptr);
	CHECK(resp->getStatusCode() == drogon::k302Found);
	std::string location = resp->getHeader("Location");
	REQUIRE(!location.empty());

	CHECK(location.find("hls/" + videoId + "/" + thumbnailId) != std::string::npos);
	CHECK(location.find("X-Amz-Signature=") != std::string::npos);
	CHECK(location.find("X-Amz-Credential=") != std::string::npos);
	CHECK(location.find("X-Amz-Expires=") != std::string::npos);
	// 存在しない動画のサムネイルをリクエストした場合404になることを確認
	videoId = "nonexistent";
	resp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId + "/thumbnails/" + thumbnailId);
	REQUIRE(resp != nullptr);
	CHECK(resp->getStatusCode() == drogon::k404NotFound);
	// もう一方のサムネイルAPIも同様にリダイレクトされることを確認
	resp = sendSyncRequest(drogon::Get, "/api/videos/" + videoIdOpt.value() + "/thumbnail");
	REQUIRE(resp != nullptr);
	CHECK(resp->getStatusCode() == drogon::k302Found);
	location = resp->getHeader("Location");
	REQUIRE(!location.empty());

	CHECK(location.find("hls/" + videoIdOpt.value() + "/thumbnail.jpg") != std::string::npos);
	CHECK(location.find("X-Amz-Signature=") != std::string::npos);
	CHECK(location.find("X-Amz-Credential=") != std::string::npos);
	CHECK(location.find("X-Amz-Expires=") != std::string::npos);
	// クリーンアップ
	CHECK(deleteVideo(videoIdOpt.value()) == true);
}

DROGON_TEST(TagTest)
{
	std::optional<std::string> videoIdOpt = postVideo("タグテスト");
	REQUIRE(videoIdOpt.has_value());
	std::string videoId = videoIdOpt.value();
	// タグを大量追加
	std::vector<std::string> tags = { "かわいい", "かっこいい", "すばらしい", "すごい", "やばい" , "すんごい" };
	for (const auto& tag : tags) {
		Json::Value tagBody;
		tagBody["tag"] = tag;
		auto tagResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId + "/tags", tagBody);
		REQUIRE(tagResp != nullptr);
		CHECK(tagResp->getStatusCode() == drogon::k200OK);
		auto tagJson = tagResp->getJsonObject();
		REQUIRE(tagJson != nullptr);
		CHECK((*tagJson)["name"].asString() == tag);
	}
	// 別の動画を用意
	std::optional<std::string> videoId2Opt = postVideo("タグテスト2");
	REQUIRE(videoId2Opt.has_value());
	std::string videoId2 = videoId2Opt.value();
	// タグを追加
	std::vector<std::string> tags2 = { "すばらしい", "すごい", "やばい", "えぐい", "終わってる" };
	for (const auto& tag : tags2) {
		Json::Value tagBody;
		tagBody["tag"] = tag;
		auto tagResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId2 + "/tags", tagBody);
		REQUIRE(tagResp != nullptr);
		CHECK(tagResp->getStatusCode() == drogon::k200OK);
		auto tagJson = tagResp->getJsonObject();
		REQUIRE(tagJson != nullptr);
		CHECK((*tagJson)["name"].asString() == tag);
	}
	// タグ検索
	std::unordered_map<std::string, std::string> queries = {
		{"query", "す"},
	};
	auto searchResp = sendSyncRequest(drogon::Get, "/api/tag", Json::Value::null, queries);
	REQUIRE(searchResp != nullptr);
	CHECK(searchResp->getStatusCode() == drogon::k200OK);
	auto searchJson = searchResp->getJsonObject();
	REQUIRE(searchJson != nullptr);
	CHECK(searchJson->isArray());
	CHECK(searchJson->size() == 3);
	std::vector<std::string> expectedTags = { "すばらしい", "すごい", "すんごい" };
	for (size_t i = 0; i < searchJson->size(); ++i) {
		std::string tagName = ((*searchJson)[static_cast<int>(i)]["name"]).asString();
		CHECK(std::ranges::contains(expectedTags, tagName));
	}
	// タグ削除
	std::vector<int> deleteTagIds;
	for (size_t i = 0; i < searchJson->size(); ++i) {
		int tagId = (*searchJson)[static_cast<int>(i)]["tag_id"].asInt();
		deleteTagIds.push_back(tagId);
	}
	for (int tagId : deleteTagIds) {
		Json::Value deleteTagBody;
		deleteTagBody["tag_id"] = tagId;
		auto deleteTagResp = sendSyncRequest(drogon::Delete, "/api/tag", deleteTagBody);
		REQUIRE(deleteTagResp != nullptr);
		CHECK(deleteTagResp->getStatusCode() == drogon::k200OK);
	}
	// タグ削除確認
	auto video1TagInfo = sendSyncRequest(drogon::Get, "/api/videos/" + videoId + "/tags");
	REQUIRE(video1TagInfo != nullptr);
	CHECK(video1TagInfo->getStatusCode() == drogon::k200OK);
	auto video1TagInfoJson = video1TagInfo->getJsonObject();
	REQUIRE(video1TagInfoJson != nullptr);
	CHECK(video1TagInfoJson->isArray());
	CHECK(video1TagInfoJson->size() == 3);
	std::vector<std::string> expectedVideo1Tags = { "かわいい", "かっこいい", "やばい" };
	for (size_t i = 0; i < video1TagInfoJson->size(); ++i) {
		std::string tagName = ((*video1TagInfoJson)[static_cast<int>(i)]["name"]).asString();
		CHECK(std::ranges::contains(expectedVideo1Tags, tagName));
	}

	auto video2TagInfo = sendSyncRequest(drogon::Get, "/api/videos/" + videoId2 + "/tags");
	REQUIRE(video2TagInfo != nullptr);
	CHECK(video2TagInfo->getStatusCode() == drogon::k200OK);
	auto video2TagInfoJson = video2TagInfo->getJsonObject();
	REQUIRE(video2TagInfoJson != nullptr);
	CHECK(video2TagInfoJson->isArray());
	CHECK(video2TagInfoJson->size() == 3);
	std::vector<std::string> expectedVideo2Tags = { "やばい", "えぐい", "終わってる" };
	for (size_t i = 0; i < video2TagInfoJson->size(); ++i) {
		std::string tagName = ((*video2TagInfoJson)[static_cast<int>(i)]["name"]).asString();
		CHECK(std::ranges::contains(expectedVideo2Tags, tagName));
	}
	// タグ検索しても削除したタグが出てこないことを確認
	searchResp = sendSyncRequest(drogon::Get, "/api/tag", Json::Value::null, queries);
	REQUIRE(searchResp != nullptr);
	CHECK(searchResp->getStatusCode() == drogon::k200OK);
	searchJson = searchResp->getJsonObject();
	REQUIRE(searchJson != nullptr);
	CHECK(searchJson->isArray());
	CHECK(searchJson->size() == 0);
	// クリーンアップ
	CHECK(deleteVideo(videoId) == true);
	CHECK(deleteVideo(videoId2) == true);
	// タグのクリーンアップ
	std::unordered_map<std::string, std::string> CleanupQueries = {
	{"query", "%"},
	};
	auto allTagsResp = sendSyncRequest(drogon::Get, "/api/tag", Json::Value::null, CleanupQueries);
	REQUIRE(allTagsResp != nullptr);
	CHECK(allTagsResp->getStatusCode() == drogon::k200OK);
	auto allTagsJson = allTagsResp->getJsonObject();
	REQUIRE(allTagsJson != nullptr);
	CHECK(allTagsJson->isArray());
	for (size_t i = 0; i < allTagsJson->size(); ++i) {
		int tagId = (*allTagsJson)[static_cast<int>(i)]["tag_id"].asInt();
		Json::Value deleteTagBody;
		deleteTagBody["tag_id"] = tagId;
		auto deleteTagResp = sendSyncRequest(drogon::Delete, "/api/tag", deleteTagBody);
		REQUIRE(deleteTagResp != nullptr);
		CHECK(deleteTagResp->getStatusCode() == drogon::k200OK);
	}
}

DROGON_TEST(AuthTest)
{
	// コメント削除権限の検証
	std::optional<std::string> videoIdOpt = postVideo("認証テスト動画");
	REQUIRE(videoIdOpt.has_value());
	std::string videoId = videoIdOpt.value();
	Json::Value commentBody;
	commentBody["content"] = "認証テストコメント";
	commentBody["timestamp"] = 5.0;
	commentBody["command"] = "white ue";
	auto commentResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId + "/comments", commentBody, {}, "comment_user");
	REQUIRE(commentResp != nullptr);
	CHECK(commentResp->getStatusCode() == drogon::k201Created);
	auto commentJson = commentResp->getJsonObject();
	REQUIRE(commentJson != nullptr);
	int commentId = (*commentJson)["comment_id"].asInt();
	// 別ユーザーでコメント削除を試みる
	Json::Value deleteCommentBody;
	deleteCommentBody["comment_id"] = commentId;
	auto deleteCommentResp = sendSyncRequest(drogon::Delete, "/api/videos/" + videoId + "/comments", deleteCommentBody, {}, "other_user");
	REQUIRE(deleteCommentResp != nullptr);
	CHECK(deleteCommentResp->getStatusCode() == drogon::k403Forbidden);
	// コメント投稿ユーザーでコメント削除を試みる
	deleteCommentResp = sendSyncRequest(drogon::Delete, "/api/videos/" + videoId + "/comments", deleteCommentBody, {}, "comment_user");
	REQUIRE(deleteCommentResp != nullptr);
	CHECK(deleteCommentResp->getStatusCode() == drogon::k200OK);
	// もう一度コメント投稿
	commentResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId + "/comments", commentBody, {}, "comment_user");
	REQUIRE(commentResp != nullptr);
	CHECK(commentResp->getStatusCode() == drogon::k201Created);
	commentJson = commentResp->getJsonObject();
	REQUIRE(commentJson != nullptr);
	commentId = (*commentJson)["comment_id"].asInt();
	deleteCommentBody["comment_id"] = commentId;
	// 動画投稿ユーザーでコメント削除を試みる
	deleteCommentResp = sendSyncRequest(drogon::Delete, "/api/videos/" + videoId + "/comments", deleteCommentBody, {}, "testuser");
	REQUIRE(deleteCommentResp != nullptr);
	CHECK(deleteCommentResp->getStatusCode() == drogon::k200OK);
	// 動画の削除の検証
	Json::Value deleteVideoBody;
	deleteVideoBody["video_id"] = videoId;
	auto deleteVideoResp = sendSyncRequest(drogon::Delete, "/api/videos", deleteVideoBody, {}, "other_user");
	REQUIRE(deleteVideoResp != nullptr);
	CHECK(deleteVideoResp->getStatusCode() == drogon::k403Forbidden);
	deleteVideoResp = sendSyncRequest(drogon::Delete, "/api/videos", deleteVideoBody, {}, "testuser");
	REQUIRE(deleteVideoResp != nullptr);
	CHECK(deleteVideoResp->getStatusCode() == drogon::k200OK);
}

DROGON_TEST(AuthRedirectTest)
{
	auto defaultResp = sendSyncRequest(drogon::Get, "/api/auth/login");
	REQUIRE(defaultResp != nullptr);
	CHECK(defaultResp->getStatusCode() == drogon::k302Found);
	const std::string defaultLocation = defaultResp->getHeader("Location");
	REQUIRE(!defaultLocation.empty());
	REQUIRE(defaultLocation.back() == '/');

	const std::string frontendOrigin = defaultLocation.substr(0, defaultLocation.size() - 1);
	const std::vector<std::string> redirectPaths = {
		"/watch/ABCD1234?tab=comments#latest",
		"/new-feature/nested/path?mode=preview#details",
		"/users/testuser/settings",
		"/?from=login"
	};
	for (const auto& redirectPath : redirectPaths) {
		for (const auto& redirect : {redirectPath, frontendOrigin + redirectPath}) {
			auto validResp = sendSyncRequest(
				drogon::Get,
				"/api/auth/login",
				Json::Value::null,
				{{"redirect", redirect}}
			);
			REQUIRE(validResp != nullptr);
			CHECK(validResp->getStatusCode() == drogon::k302Found);
			CHECK(validResp->getHeader("Location") == frontendOrigin + redirectPath);
		}
	}

	auto originOnlyResp = sendSyncRequest(
		drogon::Get,
		"/api/auth/login",
		Json::Value::null,
		{{"redirect", frontendOrigin}}
	);
	REQUIRE(originOnlyResp != nullptr);
	CHECK(originOnlyResp->getStatusCode() == drogon::k302Found);
	CHECK(originOnlyResp->getHeader("Location") == defaultLocation);

	const std::vector<std::string> unsafeRedirects = {
		"https://evil.example/",
		"https://www.youtube.com",
		frontendOrigin + ".evil.example/",
		frontendOrigin + "@evil.example/",
		frontendOrigin + "//evil.example/",
		frontendOrigin + "/\\evil.example/",
		"javascript:alert(1)",
		"data:text/html,unsafe",
		"//evil.example/",
		"///evil.example/",
		"/\\evil.example/",
		"/watch/ABCD1234\r\nLocation: https://evil.example/",
		""
	};
	for (const auto& unsafeRedirect : unsafeRedirects) {
		auto unsafeResp = sendSyncRequest(
			drogon::Get,
			"/api/auth/login",
			Json::Value::null,
			{{"redirect", unsafeRedirect}}
		);
		REQUIRE(unsafeResp != nullptr);
		CHECK(unsafeResp->getStatusCode() == drogon::k302Found);
		CHECK(unsafeResp->getHeader("Location") == defaultLocation);
	}
}

DROGON_TEST(VttTest)
{
	std::optional<std::string> videoIdOpt = postVideo("VTTテスト");
	REQUIRE(videoIdOpt.has_value());
	std::string videoId = videoIdOpt.value();
	std::string dummyVtt =
		"WEBVTT\n\n"
		"00:00:00.000 --> 00:00:05.000\n"
		"テスト字幕です。\n";
	uploadDummyFileToMinIO("hls/" + videoId + "/thumbnails.vtt", dummyVtt);
	auto resp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId + "/vtt");
	REQUIRE(resp != nullptr);
	CHECK(resp->getStatusCode() == drogon::k200OK);
	CHECK(resp->getHeader("Content-Type") == "text/vtt");
	CHECK(std::string(resp->getBody()) == dummyVtt);
	// クリーンアップ
	CHECK(deleteVideo(videoId) == true);
}
// Modalモックサーバーのグローバル変数
extern std::mutex g_modalMutex;
extern std::vector<std::string> g_modalReceivedVideoIds;
extern std::condition_variable g_modalCv;

DROGON_TEST(WebhookMinioTest)
{
	std::optional<std::string> videoIdOpt = postVideo("Webhook MinIOテスト", "MinIOにファイルがアップロードされるかのテスト", "video/mp4", false);
	REQUIRE(videoIdOpt.has_value());
	std::string videoId = videoIdOpt.value();
	// MinIOにファイルがアップロードされたことを模倣したWebhookを送信
	Json::Value payload;
	Json::Value s3Object;
	s3Object["key"] = videoId + ".mp4";
	Json::Value s3;
	s3["object"] = s3Object;
	Json::Value record;
	record["eventName"] = "s3:ObjectCreated:Put";
	record["s3"] = s3;
	payload["Records"].append(record);
	auto resp = sendSyncRequest(drogon::Post, "/webhooks/minio", payload);
	REQUIRE(resp != nullptr);
	CHECK(resp->getStatusCode() == drogon::k200OK);
	// 動画のステータスがエンコード待ちになっていることを確認
	auto dbClient = drogon::app().getDbClient();
	auto dbResult = dbClient->execSqlSync("SELECT status FROM videos WHERE video_id = ?", videoId);
	REQUIRE(dbResult.size() == 1);
	CHECK(dbResult[0]["status"].as<int>() == 1);
	// Modal宛にPOSTされたか確認（最大3秒待機）
	std::unique_lock<std::mutex> lock(g_modalMutex);
	bool found = g_modalCv.wait_for(lock, std::chrono::seconds(3), [&]() {
		return std::find(g_modalReceivedVideoIds.begin(),
			g_modalReceivedVideoIds.end(), videoId)
			!= g_modalReceivedVideoIds.end();
		});
	CHECK(found);
	// 確認後クリーンアップ
	auto it = std::find(g_modalReceivedVideoIds.begin(),
		g_modalReceivedVideoIds.end(), videoId);
	if (it != g_modalReceivedVideoIds.end())
		g_modalReceivedVideoIds.erase(it);
	// クリーンアップ
	CHECK(deleteVideo(videoId) == true);
}

DROGON_TEST(WebsocketTest)
{
	std::optional<std::string> videoIdOpt = postVideo("WebSocketテスト");
	REQUIRE(videoIdOpt.has_value());
	std::string videoId = videoIdOpt.value();

	auto wsClient = drogon::WebSocketClient::newWebSocketClient("127.0.0.1", 8080);
	auto req = drogon::HttpRequest::newHttpRequest();
	req->setPath("/ws/comments");
	req->setParameter("video_id", videoId);
	std::promise<void> connectProm;
	std::promise<std::string> messageProm;
	wsClient->setMessageHandler([&messageProm](const std::string& message,
		const drogon::WebSocketClientPtr&,
		const drogon::WebSocketMessageType&) {
			std::cout << "Recieved message from server: " << message << std::endl;
			messageProm.set_value(message);
		});
	wsClient->connectToServer(req,
		[&connectProm](drogon::ReqResult r,
			const drogon::HttpResponsePtr&,
			const drogon::WebSocketClientPtr&) {
				if (r == drogon::ReqResult::Ok) {
					std::cerr << "Connected to WebSocket" << std::endl;
					connectProm.set_value();
				} else {
					std::cerr << "Failed to connect to WebSocket" << std::endl;
				}
		});
	connectProm.get_future().get();

	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	std::string testMessage = "{\"user\":\"test_user\", \"comment\":\"テストコメント\"}";

	CommentController::broadcastToRoom(videoId, testMessage);

	std::string receivedMsg = messageProm.get_future().get();
	CHECK(receivedMsg == testMessage);
	// クリーンアップ
	CHECK(deleteVideo(videoId) == true);
}

DROGON_TEST(LIKE_TEST)
{
	std::optional<std::string> videoIdOpt = postVideo("いいねテスト");
	REQUIRE(videoIdOpt.has_value());
	std::string videoId = videoIdOpt.value();

	auto likeResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId + "/likes", Json::Value::null, {}, "testuser");
	REQUIRE(likeResp != nullptr);
	CHECK(likeResp->getStatusCode() == drogon::k201Created);
	auto likeJson = likeResp->getJsonObject();
	REQUIRE(likeJson != nullptr);
	// 同じユーザーがもう一度いいねすると弾かれることを確認
	likeResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId + "/likes", Json::Value::null, {}, "testuser");
	REQUIRE(likeResp != nullptr);
	CHECK(likeResp->getStatusCode() == drogon::k400BadRequest);
	// 別のユーザーがいいねするとカウントが増えることを確認
	likeResp = sendSyncRequest(drogon::Post, "/api/videos/" + videoId + "/likes", Json::Value::null, {}, "otheruser");
	REQUIRE(likeResp != nullptr);
	CHECK(likeResp->getStatusCode() == drogon::k201Created);
	likeJson = likeResp->getJsonObject();
	REQUIRE(likeJson != nullptr);
	// いいねしたユーザー一覧を取得
	auto getLikesResp = sendSyncRequest(drogon::Get, "/api/videos/" + videoId + "/likes");
	REQUIRE(getLikesResp != nullptr);
	CHECK(getLikesResp->getStatusCode() == drogon::k200OK);
	auto getLikesJson = getLikesResp->getJsonObject();
	REQUIRE(getLikesJson != nullptr);
	CHECK(getLikesJson->isArray());
	std::vector<std::string> expectedUsers = { "testuser", "otheruser" };
	for (size_t i = 0; i < getLikesJson->size(); ++i) {
		std::string username = ((*getLikesJson)[static_cast<int>(i)]).asString();
		CHECK(std::ranges::contains(expectedUsers, username));
	}
	// いいねを取り消す
	auto unlikeResp = sendSyncRequest(drogon::Delete, "/api/videos/" + videoId + "/likes", Json::Value::null, {}, "testuser");
	REQUIRE(unlikeResp != nullptr);
	CHECK(unlikeResp->getStatusCode() == drogon::k200OK);
	auto unlikeJson = unlikeResp->getJsonObject();
	REQUIRE(unlikeJson != nullptr);
	// もう一度いいねを取り消すと弾かれることを確認
	unlikeResp = sendSyncRequest(drogon::Delete, "/api/videos/" + videoId + "/likes", Json::Value::null, {}, "testuser");
	REQUIRE(unlikeResp != nullptr);
	CHECK(unlikeResp->getStatusCode() == drogon::k400BadRequest);

	// クリーンアップ
	CHECK(deleteVideo(videoId) == true);
}

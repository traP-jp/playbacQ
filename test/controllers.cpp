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

void deleteRedisKeySync(const std::string& key) {
	auto redisClient = drogon::app().getRedisClient();
	std::promise<void> prom;
	auto fut = prom.get_future();

	redisClient->execCommandAsync(
		[&prom](const drogon::nosql::RedisResult&) {
			prom.set_value();
		},
		[&prom](const std::exception& e) {
			std::cerr << "Redis Error: " << e.what() << std::endl;
			prom.set_value();
		},
		"DEL %s", key.c_str()
	);
	fut.get();
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

DROGON_TEST(ExternalVideoRefreshTest)
{
	auto dbClient = drogon::app().getDbClient();
	const std::string archivedId = "refresh-test-archived";
	const std::string activeId = "refresh-test-active";
	const std::string premiereId = "refresh-test-premiere";
	const std::string dayLongId = "refresh-test-day-long";
	const std::string normalId = "refresh-test-normal";
	const std::string metadataId = "refresh-test-metadata";
	const std::string partialId = "refresh-test-partial";
	const std::string failedId = "refresh-test-failed";
	const std::string missingUpstreamId = "refresh-test-missing-upstream";
	const std::string internalId = "refresh-test-internal";

	dbClient->execSqlSync("DELETE FROM videos WHERE video_id LIKE 'refresh-test-%'");
	auto insertVideo = [&](const std::string& id,
		const std::string& title,
		const std::string& description,
		const std::string& videoUrl,
		int duration,
		int isExternal,
		const std::string& type) {
		dbClient->execSqlSync(
			"INSERT INTO videos "
			"(video_id, user_id, title, description, video_url, view_count, duration, "
			"like_count, status, is_external, type) "
			"VALUES (?, 'testuser', ?, ?, ?, 7, ?, 8, 2, ?, ?)",
			id,
			title,
			description,
			videoUrl,
			duration,
			isExternal,
			type
		);
	};

	insertVideo(archivedId, "アーカイブ前タイトル", "アーカイブ前説明", "MOCKVIDEO01", 0, 1, "youtube live");
	insertVideo(activeId, "ライブタイトル", "ライブ説明", "LIVEVIDEO01", 17, 1, "youtube live");
	insertVideo(premiereId, "プレミアタイトル", "プレミア説明", "PREMVIDEO01", 3600, 1, "youtube live");
	insertVideo(dayLongId, "長時間タイトル", "長時間説明", "DAYVIDEO001", 0, 1, "youtube live");
	insertVideo(normalId, "通常タイトル", "通常説明", "MOCKVIDEO01", 123, 1, "youtube");
	insertVideo(metadataId, "同期前タイトル", "同期前説明", "MOCKVIDEO01", 77, 1, "youtube");
	insertVideo(partialId, "部分同期前タイトル", "部分同期前説明", "PARTVIDEO01", 88, 1, "youtube");
	insertVideo(failedId, "失敗前タイトル", "失敗前説明", "FAILVIDEO01", 99, 1, "youtube live");
	insertVideo(missingUpstreamId, "欠落前タイトル", "欠落前説明", "MISSVIDEO01", 111, 1, "youtube");
	insertVideo(internalId, "内部タイトル", "内部説明", "/watch/internal", 120, 0, "internal");

	const std::vector<std::string> durationIds = {
		archivedId,
		activeId,
		premiereId,
		dayLongId,
		failedId,
	};
	for (const auto& id : durationIds) {
		deleteRedisKeySync("external-video:duration-refresh:" + id);
	}
	g_youtubeVideosListCalls.store(0, std::memory_order_relaxed);

	// GETはDB取得だけで、YouTube APIもRedisクールダウンも変更しない。
	auto getResponse = sendSyncRequest(drogon::Get, "/api/videos/" + activeId);
	REQUIRE(getResponse != nullptr);
	CHECK(getResponse->getStatusCode() == drogon::k200OK);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 0);
	CHECK(!getRedisValueSync("external-video:duration-refresh:" + activeId));

	// 再生時間更新APIは認証を要求するが、投稿者以外でも実行できる。
	auto unauthorizedDuration = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + activeId + "/duration/refresh",
		Json::Value::null,
		{},
		""
	);
	REQUIRE(unauthorizedDuration != nullptr);
	CHECK(unauthorizedDuration->getStatusCode() == drogon::k401Unauthorized);

	auto archivedResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + archivedId + "/duration/refresh",
		Json::Value::null,
		{},
		"otheruser"
	);
	REQUIRE(archivedResponse != nullptr);
	CHECK(archivedResponse->getStatusCode() == drogon::k200OK);
	auto archivedJson = archivedResponse->getJsonObject();
	REQUIRE(archivedJson != nullptr);
	CHECK((*archivedJson)["status"].asString() == "updated");
	CHECK((*archivedJson)["quota_units"].asInt() == 1);
	CHECK((*archivedJson)["video"]["duration"].asInt() == 123);
	CHECK((*archivedJson)["video"]["type"].asString() == "youtube");
	CHECK((*archivedJson)["video"]["title"].asString() == "アーカイブ前タイトル");
	CHECK((*archivedJson)["video"]["description"].asString() == "アーカイブ前説明");
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 1);
	CHECK(getRedisValueSync("external-video:duration-refresh:" + archivedId) == "1");
	{
		std::lock_guard<std::mutex> lock(g_youtubeRequestMutex);
		CHECK(g_lastYoutubeVideoParts == "snippet,contentDetails");
		CHECK(g_lastYoutubeVideoIds == "MOCKVIDEO01");
	}

	auto archivedRows = dbClient->execSqlSync(
		"SELECT title, description, duration, type, view_count, like_count "
		"FROM videos WHERE video_id = ?",
		archivedId
	);
	REQUIRE(archivedRows.size() == 1);
	CHECK(archivedRows[0]["title"].as<std::string>() == "アーカイブ前タイトル");
	CHECK(archivedRows[0]["description"].as<std::string>() == "アーカイブ前説明");
	CHECK(archivedRows[0]["duration"].as<int>() == 123);
	CHECK(archivedRows[0]["type"].as<std::string>() == "youtube");
	CHECK(archivedRows[0]["view_count"].as<int>() == 7);
	CHECK(archivedRows[0]["like_count"].as<int>() == 8);

	auto activeResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + activeId + "/duration/refresh",
		Json::Value::null,
		{},
		"otheruser"
	);
	REQUIRE(activeResponse != nullptr);
	CHECK(activeResponse->getStatusCode() == drogon::k200OK);
	auto activeJson = activeResponse->getJsonObject();
	REQUIRE(activeJson != nullptr);
	CHECK((*activeJson)["status"].asString() == "unchanged");
	CHECK((*activeJson)["reason"].asString() == "broadcast_not_archived");
	CHECK((*activeJson)["quota_units"].asInt() == 1);
	CHECK((*activeJson)["video"]["duration"].asInt() == 17);
	CHECK((*activeJson)["video"]["type"].asString() == "youtube live");
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 2);
	CHECK(getRedisValueSync("external-video:duration-refresh:" + activeId) == "1");

	auto cooldownResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + activeId + "/duration/refresh",
		Json::Value::null,
		{},
		"otheruser"
	);
	REQUIRE(cooldownResponse != nullptr);
	CHECK(cooldownResponse->getStatusCode() == drogon::k200OK);
	auto cooldownJson = cooldownResponse->getJsonObject();
	REQUIRE(cooldownJson != nullptr);
	CHECK((*cooldownJson)["status"].asString() == "cooldown");
	CHECK((*cooldownJson)["quota_units"].asInt() == 0);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 2);

	// プレミア公開前・公開中に取得済みの再生時間は上書きしない。
	auto premiereResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + premiereId + "/duration/refresh"
	);
	REQUIRE(premiereResponse != nullptr);
	CHECK(premiereResponse->getStatusCode() == drogon::k200OK);
	auto premiereJson = premiereResponse->getJsonObject();
	REQUIRE(premiereJson != nullptr);
	CHECK((*premiereJson)["status"].asString() == "unchanged");
	CHECK((*premiereJson)["reason"].asString() == "broadcast_not_archived");
	CHECK((*premiereJson)["video"]["duration"].asInt() == 3600);
	CHECK((*premiereJson)["video"]["type"].asString() == "youtube live");
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 3);

	// ISO 8601の日を含むdurationも秒へ変換する。
	auto dayLongResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + dayLongId + "/duration/refresh"
	);
	REQUIRE(dayLongResponse != nullptr);
	CHECK(dayLongResponse->getStatusCode() == drogon::k200OK);
	auto dayLongJson = dayLongResponse->getJsonObject();
	REQUIRE(dayLongJson != nullptr);
	CHECK((*dayLongJson)["status"].asString() == "updated");
	CHECK((*dayLongJson)["video"]["duration"].asInt() == 93784);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 4);

	// 通常のYouTube動画はYouTube APIを呼ばずに終了する。
	auto normalResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + normalId + "/duration/refresh"
	);
	REQUIRE(normalResponse != nullptr);
	CHECK(normalResponse->getStatusCode() == drogon::k200OK);
	auto normalJson = normalResponse->getJsonObject();
	REQUIRE(normalJson != nullptr);
	CHECK((*normalJson)["status"].asString() == "unchanged");
	CHECK((*normalJson)["reason"].asString() == "not_live_video");
	CHECK((*normalJson)["quota_units"].asInt() == 0);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 4);

	// YouTube API失敗時はDBを変更せず、クールダウンで連続リトライを防ぐ。
	auto failedDurationResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + failedId + "/duration/refresh"
	);
	REQUIRE(failedDurationResponse != nullptr);
	CHECK(failedDurationResponse->getStatusCode() == drogon::k502BadGateway);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 5);
	CHECK(getRedisValueSync("external-video:duration-refresh:" + failedId) == "1");
	auto failedDurationRows = dbClient->execSqlSync(
		"SELECT duration, type FROM videos WHERE video_id = ?",
		failedId
	);
	REQUIRE(failedDurationRows.size() == 1);
	CHECK(failedDurationRows[0]["duration"].as<int>() == 99);
	CHECK(failedDurationRows[0]["type"].as<std::string>() == "youtube live");

	auto failedCooldownResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + failedId + "/duration/refresh"
	);
	REQUIRE(failedCooldownResponse != nullptr);
	CHECK(failedCooldownResponse->getStatusCode() == drogon::k200OK);
	auto failedCooldownJson = failedCooldownResponse->getJsonObject();
	REQUIRE(failedCooldownJson != nullptr);
	CHECK((*failedCooldownJson)["status"].asString() == "cooldown");
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 5);

	auto internalDurationResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + internalId + "/duration/refresh"
	);
	REQUIRE(internalDurationResponse != nullptr);
	CHECK(internalDurationResponse->getStatusCode() == drogon::k409Conflict);
	auto missingDurationResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/refresh-test-not-found/duration/refresh"
	);
	REQUIRE(missingDurationResponse != nullptr);
	CHECK(missingDurationResponse->getStatusCode() == drogon::k404NotFound);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 5);

	// タイトルだけを明示的に同期し、その他の動画情報は保持する。
	Json::Value titleBody;
	titleBody["fields"] = Json::Value(Json::arrayValue);
	titleBody["fields"].append("title");
	auto metadataResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + metadataId + "/metadata/refresh",
		titleBody
	);
	REQUIRE(metadataResponse != nullptr);
	CHECK(metadataResponse->getStatusCode() == drogon::k200OK);
	auto metadataJson = metadataResponse->getJsonObject();
	REQUIRE(metadataJson != nullptr);
	CHECK((*metadataJson)["status"].asString() == "updated");
	CHECK((*metadataJson)["quota_units"].asInt() == 1);
	CHECK((*metadataJson)["synced_fields"].size() == 1);
	CHECK((*metadataJson)["synced_fields"][0].asString() == "title");
	CHECK((*metadataJson)["skipped_fields"].empty());
	CHECK((*metadataJson)["video"]["title"].asString() == "YouTube同期タイトル");
	CHECK((*metadataJson)["video"]["description"].asString() == "同期前説明");
	CHECK((*metadataJson)["video"]["duration"].asInt() == 77);
	CHECK((*metadataJson)["video"]["type"].asString() == "youtube");
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 6);

	auto metadataRows = dbClient->execSqlSync(
		"SELECT title, description, duration, type, view_count, like_count "
		"FROM videos WHERE video_id = ?",
		metadataId
	);
	REQUIRE(metadataRows.size() == 1);
	CHECK(metadataRows[0]["title"].as<std::string>() == "YouTube同期タイトル");
	CHECK(metadataRows[0]["description"].as<std::string>() == "同期前説明");
	CHECK(metadataRows[0]["duration"].as<int>() == 77);
	CHECK(metadataRows[0]["type"].as<std::string>() == "youtube");
	CHECK(metadataRows[0]["view_count"].as<int>() == 7);
	CHECK(metadataRows[0]["like_count"].as<int>() == 8);

	// メタデータ同期は投稿者だけに許可する。
	auto forbiddenMetadataResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + metadataId + "/metadata/refresh",
		titleBody,
		{},
		"otheruser"
	);
	REQUIRE(forbiddenMetadataResponse != nullptr);
	CHECK(forbiddenMetadataResponse->getStatusCode() == drogon::k403Forbidden);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 6);
	auto unauthorizedMetadataResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + metadataId + "/metadata/refresh",
		titleBody,
		{},
		""
	);
	REQUIRE(unauthorizedMetadataResponse != nullptr);
	CHECK(unauthorizedMetadataResponse->getStatusCode() == drogon::k401Unauthorized);

	// YouTubeレスポンスで欠落したフィールドだけを更新対象から外す。
	Json::Value bothFieldsBody;
	bothFieldsBody["fields"] = Json::Value(Json::arrayValue);
	bothFieldsBody["fields"].append("title");
	bothFieldsBody["fields"].append("description");
	auto partialResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + partialId + "/metadata/refresh",
		bothFieldsBody
	);
	REQUIRE(partialResponse != nullptr);
	CHECK(partialResponse->getStatusCode() == drogon::k200OK);
	auto partialJson = partialResponse->getJsonObject();
	REQUIRE(partialJson != nullptr);
	CHECK((*partialJson)["status"].asString() == "partial");
	CHECK((*partialJson)["synced_fields"].size() == 1);
	CHECK((*partialJson)["synced_fields"][0].asString() == "title");
	CHECK((*partialJson)["skipped_fields"].size() == 1);
	CHECK((*partialJson)["skipped_fields"][0].asString() == "description");
	CHECK((*partialJson)["video"]["title"].asString() == "YouTube同期タイトル");
	CHECK((*partialJson)["video"]["description"].asString() == "部分同期前説明");
	CHECK((*partialJson)["video"]["duration"].asInt() == 88);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 7);

	// フィールド指定は省略・空・重複・不明フィールドを拒否する。
	Json::Value emptyFieldsBody;
	emptyFieldsBody["fields"] = Json::Value(Json::arrayValue);
	Json::Value duplicateFieldsBody;
	duplicateFieldsBody["fields"] = Json::Value(Json::arrayValue);
	duplicateFieldsBody["fields"].append("title");
	duplicateFieldsBody["fields"].append("title");
	Json::Value unsupportedFieldBody;
	unsupportedFieldBody["fields"] = Json::Value(Json::arrayValue);
	unsupportedFieldBody["fields"].append("duration");
	Json::Value additionalPropertyBody = titleBody;
	additionalPropertyBody["unexpected"] = true;
	const std::vector<Json::Value> invalidMetadataBodies = {
		Json::Value(Json::objectValue),
		Json::Value(Json::arrayValue),
		emptyFieldsBody,
		duplicateFieldsBody,
		unsupportedFieldBody,
		additionalPropertyBody,
	};
	for (const auto& invalidBody : invalidMetadataBodies) {
		auto invalidResponse = sendSyncRequest(
			drogon::Post,
			"/api/ex-videos/" + metadataId + "/metadata/refresh",
			invalidBody
		);
		REQUIRE(invalidResponse != nullptr);
		CHECK(invalidResponse->getStatusCode() == drogon::k400BadRequest);
	}
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 7);

	auto failedMetadataResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + failedId + "/metadata/refresh",
		titleBody
	);
	REQUIRE(failedMetadataResponse != nullptr);
	CHECK(failedMetadataResponse->getStatusCode() == drogon::k502BadGateway);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 8);
	auto missingUpstreamResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + missingUpstreamId + "/metadata/refresh",
		titleBody
	);
	REQUIRE(missingUpstreamResponse != nullptr);
	CHECK(missingUpstreamResponse->getStatusCode() == drogon::k502BadGateway);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 9);

	auto internalMetadataResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/" + internalId + "/metadata/refresh",
		titleBody
	);
	REQUIRE(internalMetadataResponse != nullptr);
	CHECK(internalMetadataResponse->getStatusCode() == drogon::k409Conflict);
	auto missingMetadataResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/refresh-test-not-found/metadata/refresh",
		titleBody
	);
	REQUIRE(missingMetadataResponse != nullptr);
	CHECK(missingMetadataResponse->getStatusCode() == drogon::k404NotFound);
	CHECK(g_youtubeVideosListCalls.load(std::memory_order_relaxed) == 9);

	// 旧バッチAPIは削除されている。
	auto removedBatchResponse = sendSyncRequest(
		drogon::Post,
		"/api/ex-videos/sync",
		titleBody
	);
	REQUIRE(removedBatchResponse != nullptr);
	CHECK(removedBatchResponse->getStatusCode() == drogon::k404NotFound);

	for (const auto& id : durationIds) {
		deleteRedisKeySync("external-video:duration-refresh:" + id);
	}
	dbClient->execSqlSync("DELETE FROM videos WHERE video_id LIKE 'refresh-test-%'");
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

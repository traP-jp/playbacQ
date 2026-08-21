#include "api_videos.h"
#include <drogon/orm/Exception.h>
#include <drogon/orm/CoroMapper.h>
#include <drogon/orm/Criteria.h>
#include <drogon/nosql/RedisClient.h>
#include <json/json.h>
#include <string>
#include <optional>
#include <iostream>
#include <cstdlib>
#include <regex>
#include <format>
#include <ranges>
#include <string_view>
#include "../models/Videos.h"
#include "../models/Comments.h"
#include "../models/Tags.h"
#include "../models/VideoTags.h"
#include "../models/VideoLikes.h"
#include "../plugins/S3Plugin.h"
#include "../plugins/Token.h"
#include "../plugins/YoutubeAPI.h"
#include "Status.h"

using namespace api;

drogon::Task<drogon::HttpResponsePtr> videos::getVideos(HttpRequestPtr req) {
	std::optional<std::string> userId = req->getOptionalParameter<std::string>("userId");
	std::optional<std::string> search = req->getOptionalParameter<std::string>("search");
	std::optional<std::string> tag = req->getOptionalParameter<std::string>("tag");
	std::optional<std::string> sortby = req->getOptionalParameter<std::string>("sortby");
	std::optional<int> order = req->getOptionalParameter<int>("order");

	drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
	drogon::orm::Criteria criteria;

	criteria = criteria && drogon::orm::Criteria(drogon_model::playbacq::Videos::Cols::_status, drogon::orm::CompareOperator::EQ, (uint8_t)Status::completed);
	if (userId.has_value() && !userId.value().empty()) {
		criteria = criteria && drogon::orm::Criteria(drogon_model::playbacq::Videos::Cols::_user_id, drogon::orm::CompareOperator::EQ, userId.value());
	}
	if (search.has_value() && !search.value().empty()) {
		if (search.value().length() > 1024) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
			resp->setBody("Search query is too long");
			co_return resp;
		}
		std::string searchStr = "%" + search.value() + "%";
		auto titleCriteria = drogon::orm::Criteria(drogon_model::playbacq::Videos::Cols::_title, drogon::orm::CompareOperator::Like, searchStr);
		auto descriptionCriteria = drogon::orm::Criteria(drogon_model::playbacq::Videos::Cols::_description, drogon::orm::CompareOperator::Like, searchStr);
		criteria = criteria && (titleCriteria || descriptionCriteria);
	}
	if (tag.has_value() && !tag.value().empty()) {
		drogon::orm::CoroMapper<drogon_model::playbacq::Tags> tagMapper(drogon::app().getDbClient());
		try {
			auto tagObj = co_await tagMapper.findOne(drogon::orm::Criteria(drogon_model::playbacq::Tags::Cols::_name, drogon::orm::CompareOperator::EQ, tag.value()));
			drogon::orm::CoroMapper<drogon_model::playbacq::VideoTags> videoTagMapper(drogon::app().getDbClient());
			auto videoTags = co_await videoTagMapper.findBy(drogon::orm::Criteria(drogon_model::playbacq::VideoTags::Cols::_tag_id, drogon::orm::CompareOperator::EQ, *tagObj.getTagId()));
			if (videoTags.empty()) {
				auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value(Json::arrayValue));
				resp->setStatusCode(drogon::HttpStatusCode::k200OK);
				co_return resp;
			}
			std::vector<std::string> videoIds;
			for (const auto& videoTag : videoTags) {
				videoIds.push_back(*videoTag.getVideoId());
			}
			criteria = criteria && drogon::orm::Criteria(drogon_model::playbacq::Videos::Cols::_video_id, drogon::orm::CompareOperator::In, videoIds);

		}
		catch (const std::exception& e) {
			std::cerr << "DB Error: " << e.what() << std::endl;
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
			resp->setBody("Failed to retrieve videos: " + std::string(e.what()));
			co_return resp;
		}
	}
	if (sortby.has_value()) {
		auto sort_order = (order.has_value() && order.value() == 0) ? drogon::orm::SortOrder::DESC : drogon::orm::SortOrder::ASC;
		if (sortby.value() == "created_at") {
			mapper.orderBy(drogon_model::playbacq::Videos::Cols::_created_at, sort_order);
		} else if (sortby.value() == "title") {
			mapper.orderBy(drogon_model::playbacq::Videos::Cols::_title, sort_order);
		} else if (sortby.value() == "view_count") {
			mapper.orderBy(drogon_model::playbacq::Videos::Cols::_view_count, sort_order);
		} else if (sortby.value() == "duration") {
			mapper.orderBy(drogon_model::playbacq::Videos::Cols::_duration, sort_order);
		} else {
			// TODO : デフォルト値をいい感じにする
			mapper.orderBy(drogon_model::playbacq::Videos::Cols::_created_at, drogon::orm::SortOrder::DESC);
		}
	} else {
		// デフォルトは作成日時の降順
		mapper.orderBy(drogon_model::playbacq::Videos::Cols::_created_at, drogon::orm::SortOrder::DESC);
	}

	// DB検索開始
	try {
		auto videosList = co_await mapper.findBy(criteria);
		Json::Value jsonResponse(Json::arrayValue);
		for (const auto& video : videosList) {
			jsonResponse.append(video.toJson());
		}
		co_return drogon::HttpResponse::newHttpJsonResponse(jsonResponse);
	}
	catch (const std::exception& e) {
		std::cerr << "DB Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to retrieve videos: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::postVideos(HttpRequestPtr req) {
	auto jsonPtr = req->getJsonObject();
	if (!jsonPtr) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
		resp->setBody("Invalid JSON format");
		co_return resp;
	}
	try {
		drogon_model::playbacq::Videos newVideo;
		// 動画IDは乱数。垓一衝突したらエラーを投げるはずなのでユーザーがもう一度リクエストすればよい。
		std::string videoId = drogon::utils::genRandomString(11);
		newVideo.setVideoId(videoId);
		newVideo.setCreatedAt(trantor::Date::now());
		newVideo.setViewCount(0);

		// JSONから動画情報を設定
		newVideo.setTitle(jsonPtr->get("title", "NULL").asString());
		newVideo.setDescription(jsonPtr->get("description", "NULL").asString());
		newVideo.setUserId(req->getAttributes()->get<std::string>("userId"));
		newVideo.setStatus((uint8_t)Status::pending);

		// 動画ファイルのContent-Typeを検証
		std::string contentType = jsonPtr->get("content_type", "NULL").asString();
		if (!contentType.starts_with("video/")) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
			resp->setBody("Only video files are allowed (uploaded type was: " + contentType + ")");
			co_return resp;
		}

		// S3の事前署名URLを生成
		auto s3Plugin = drogon::app().getPlugin<S3Plugin>();
		std::string uploadUrl = s3Plugin->genPresignedUrl(videoId + ".mp4", contentType, "videofiles");
		std::string thumbUploadUrl = s3Plugin->genPresignedUrl(videoId + ".jpg", "image/jpeg", "thumbnails");

		// 動画URLを設定
		auto customConfig = drogon::app().getCustomConfig();
		std::string baseUrl = customConfig["baseUrl"].asString();
		newVideo.setVideoUrl(baseUrl + "/watch/" + videoId);
		newVideo.setThumbnailUrl("/api/videos/" + videoId + "/thumbnail");

		drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
		co_await mapper.insert(newVideo);

		Json::Value jsonResponse = newVideo.toJson();
		jsonResponse["uploadUrl"] = uploadUrl;
		jsonResponse["thumbUploadUrl"] = thumbUploadUrl;

		auto resp = drogon::HttpResponse::newHttpJsonResponse(jsonResponse);
		resp->setStatusCode(drogon::HttpStatusCode::k201Created);
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to create video: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::deleteVideo(HttpRequestPtr req) {
	auto jsonPtr = req->getJsonObject();
	if (!jsonPtr) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
		resp->setBody("Invalid JSON format");
		co_return resp;
	}
	std::string videoId = jsonPtr->get("video_id", "").asString();
	if (videoId.empty()) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
		resp->setBody("Missing video_id");
		co_return resp;
	}
	try {
		drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
		auto video = co_await mapper.findByPrimaryKey(videoId);
		if (*video.getUserId() != req->getAttributes()->get<std::string>("userId")) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k403Forbidden);
			resp->setBody("You are not the owner of this video");
			co_return resp;
		}
		// コメントを削除
		drogon::orm::CoroMapper<drogon_model::playbacq::Comments> commentMapper(drogon::app().getDbClient());
		co_await commentMapper.deleteBy(drogon::orm::Criteria(drogon_model::playbacq::Comments::Cols::_video_id, drogon::orm::CompareOperator::EQ, videoId));
		// VideoTagsを削除
		drogon::orm::CoroMapper<drogon_model::playbacq::VideoTags> videoTagMapper(drogon::app().getDbClient());
		co_await videoTagMapper.deleteBy(drogon::orm::Criteria(drogon_model::playbacq::VideoTags::Cols::_video_id, drogon::orm::CompareOperator::EQ, videoId));
		// MinIO上の実体をフォルダごと削除
		auto s3Plugin = drogon::app().getPlugin<S3Plugin>();
		try {
			s3Plugin->deleteFolder("hls/" + videoId + "/");
		}
		catch (const std::exception& e) {
			std::cerr << "S3 Error: " << e.what() << std::endl;
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
			resp->setBody("Failed to delete video files: " + std::string(e.what()));
			co_return resp;
		}
		// DBから動画情報を削除
		co_await mapper.deleteByPrimaryKey(videoId);

		Json::Value jsonResponse;
		jsonResponse["video_id"] = videoId;
		jsonResponse["message"] = "Video deleted successfully";
		auto resp = drogon::HttpResponse::newHttpJsonResponse(jsonResponse);
		resp->setStatusCode(drogon::HttpStatusCode::k200OK);
		co_return resp;
	}
	catch (const drogon::orm::UnexpectedRows& e) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
		resp->setBody("Video not found");
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "DB Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to delete video: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::postExVideo(HttpRequestPtr req) {
	auto jsonPtr = req->getJsonObject();
	if (!jsonPtr) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
		resp->setBody("Invalid JSON format");
		co_return resp;
	}
	try {
		drogon_model::playbacq::Videos newVideo;
		std::string videoId = drogon::utils::genRandomString(11);
		newVideo.setVideoId(videoId);
		newVideo.setCreatedAt(trantor::Date::now());
		newVideo.setViewCount(0);
		// JSONから動画情報を設定
		newVideo.setIsExternal(1);
		// URLからYoutubeの動画IDを抽出(将来的に別の動画サイトに対応する場合はここで判定する。)
		const std::string videoUrl = jsonPtr->get("url", "").asString();
		std::regex youtubeRegex(
			R"(^(?:https?:\/\/)?(?:www\.|m\.)?(?:youtu\.be\/|youtube\.com\/(?:embed\/|v\/|watch\?v=|watch\?.+&v=|shorts\/|live\/))([\w-]{11})(?:\S+)?$)"
		);
		std::smatch match;
		std::string youtubeID;
		if (std::regex_match(videoUrl, match, youtubeRegex) && match.size() > 1) {
			youtubeID = match[1].str();
			newVideo.setVideoUrl(youtubeID);
		} else {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
			resp->setBody("Invalid YouTube URL");
			co_return resp;
		}
		newVideo.setThumbnailUrl("https://img.youtube.com/vi/" + youtubeID + "/hqdefault.jpg");
		// Youtube APIを使って動画情報を取得する。
		const char* apiKeyEnv = std::getenv("YOUTUBE_API_KEY");
		if (apiKeyEnv == nullptr) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
			resp->setBody("YouTube API key is not set");
			co_return resp;
		}
		const std::string apiKey = (apiKeyEnv != nullptr) ? std::string(apiKeyEnv) : "";
		auto videoInfoRaw = co_await YoutubeAPI::fetchVideoInfo(youtubeID, apiKey);
		if (!videoInfoRaw) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
			resp->setBody("Failed to fetch video info from YouTube API");
			co_return resp;
		}
		const Json::Value videoInfo = videoInfoRaw.value();
		// 現時点ではYoutubeのみ対応
		if (videoInfo.get("isLive", false).asBool() || videoInfo.get("isUpcoming", false).asBool()) {
			newVideo.setType("youtube live");
		} else {
			newVideo.setType("youtube");
		}
		// titleが空ならデフォルト値を設定する。
		if (!jsonPtr->isMember("title") || jsonPtr->get("title", "").asString().empty()) {
			newVideo.setTitle(videoInfo.get("title", "NULL").asString());
		} else {
			newVideo.setTitle(jsonPtr->get("title", "NULL").asString());
		}
		// descriptionが空ならデフォルト値を設定する。
		if (!jsonPtr->isMember("description") || jsonPtr->get("description", "").asString().empty()) {
			newVideo.setDescription(videoInfo.get("description", "NULL").asString());
		} else {
			newVideo.setDescription(jsonPtr->get("description", "NULL").asString());
		}
		// 動画の長さをYoutubeから取得する。
		newVideo.setDuration(videoInfo.get("duration", 0).asInt());

		newVideo.setUserId(req->getAttributes()->get<std::string>("userId"));
		newVideo.setStatus((uint8_t)Status::completed);

		drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
		co_await mapper.insert(newVideo);

		Json::Value jsonResponse = newVideo.toJson();
		auto resp = drogon::HttpResponse::newHttpJsonResponse(jsonResponse);
		resp->setStatusCode(drogon::HttpStatusCode::k201Created);
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to create video: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::getVideo([[maybe_unused]] HttpRequestPtr req, std::string id) {
	drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
	try {
		auto video = co_await mapper.findByPrimaryKey(id);
		auto resp = drogon::HttpResponse::newHttpJsonResponse(video.toJson());
		co_return resp;
	}
	catch (const drogon::orm::UnexpectedRows& e) {
		// Not found 404
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
		resp->setBody("Video not found");
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "DB Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to retrieve video: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::patchVideo([[maybe_unused]] HttpRequestPtr req, std::string id) {
	auto jsonPtr = req->getJsonObject();
	if (!jsonPtr || !jsonPtr->isObject() || jsonPtr->empty()) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
		resp->setBody("Request body must be a non-empty JSON object");
		co_return resp;
	}

	for (const auto& key : jsonPtr->getMemberNames()) {
		if ((key != "title" && key != "description") || !(*jsonPtr)[key].isString()) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
			resp->setBody("Only string fields 'title' and 'description' can be updated");
			co_return resp;
		}
	}

	drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
	try {
		auto video = co_await mapper.findByPrimaryKey(id);
		if (*video.getUserId() != req->getAttributes()->get<std::string>("userId")) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k403Forbidden);
			resp->setBody("You are not the owner of this video");
			co_return resp;
		}
		if (jsonPtr->isMember("title")) {
			video.setTitle((*jsonPtr)["title"].asString());
		}
		if (jsonPtr->isMember("description")) {
			video.setDescription((*jsonPtr)["description"].asString());
		}
		co_await mapper.update(video);

		auto resp = drogon::HttpResponse::newHttpJsonResponse(video.toJson());
		resp->setStatusCode(drogon::HttpStatusCode::k200OK);
		co_return resp;
	}
	catch (const drogon::orm::UnexpectedRows& e) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
		resp->setBody("Video not found");
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "DB Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to update video: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::getVideoProgress([[maybe_unused]] HttpRequestPtr req, std::string id) {
	// Statusの確認
	std::cout << "Checking progress for video ID: " << id << std::endl;
	drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
	Json::Value jsonResponse;
	try {
		auto video = co_await mapper.findByPrimaryKey(id);
		if (*video.getStatus() != (uint8_t)Status::processing) {
			jsonResponse["progress"] = Json::nullValue;
			jsonResponse["status"] = *video.getStatus();
			auto resp = drogon::HttpResponse::newHttpJsonResponse(jsonResponse);
			co_return resp;
		}
	}
	catch (const drogon::orm::UnexpectedRows& e) {
		// Not found 404
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
		resp->setBody("Video not found");
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "DB Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to retrieve video: " + std::string(e.what()));
		co_return resp;
	}
	// processing中の場合進捗も取得
	auto redis = drogon::app().getRedisClient();
	if (!redis) {
		std::cerr << "Failed to get Redis client" << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to get Redis client");
		co_return resp;
	}
	try {
		auto result = co_await redis->execCommandCoro("GET video:progress:%s", id.c_str());
		if (!result.isNil()) {
			std::string progressStr = result.asString();
			try {
				jsonResponse["progress"] = std::max(0, std::min(100, std::stoi(progressStr)));
			}
			catch (const std::exception& e) {
				std::cerr << "Invalid progress value in Redis: " << progressStr << std::endl;
				jsonResponse["progress"] = Json::nullValue;
			}
		} else {
			jsonResponse["progress"] = Json::nullValue;
		}
		jsonResponse["status"] = (uint8_t)Status::processing;
		auto resp = drogon::HttpResponse::newHttpJsonResponse(jsonResponse);
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "Redis Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to retrieve progress: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::getVideoPlayM3u8([[maybe_unused]] HttpRequestPtr req, std::string id) {
	// 動画URLの確認
	std::cout << "Checking play URL for video ID: " << id << std::endl;
	drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
	Json::Value jsonResponse;
	try {
		auto video = co_await mapper.findByPrimaryKey(id);
		if (*video.getStatus() != (uint8_t)Status::completed) {
			auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value());
			co_return resp;
		}
		auto s3Plugin = drogon::app().getPlugin<S3Plugin>();
		std::string base_path = "hls/" + id + "/";
		// S3 SDKで直接m3u8ファイルを取得
		std::string original_m3u8 = s3Plugin->getObject(base_path + "output.m3u8");
		std::cout << "Successfully fetched m3u8 from MinIO for video ID: " << id << std::endl;

		std::string rewritten_m3u8;
		for (const auto& line_range : original_m3u8 | std::views::split('\n')) {
			std::string_view line(line_range.begin(), line_range.end());
			// Windowsの改行コードに対応するため、行末の'\r'を削除
			if (!line.empty() && line.back() == '\r') {
				line.remove_suffix(1);
			}
			if (line.empty()) continue;
			// 署名付きURLに書き換える
			if (line.starts_with("#EXT-X-MAP:URI=\"")) {
				auto uri_start = line.find("\"") + 1;
				auto uri_end = line.find("\"", uri_start);
				if (uri_start != std::string_view::npos && uri_end != std::string_view::npos) {
					std::string_view filename{ line.substr(uri_start, uri_end - uri_start) };
					rewritten_m3u8 += std::format("#EXT-X-MAP:URI=\"{}\"{}\n",
						s3Plugin->genPresignedGetUrl(base_path + std::string(filename)),
						line.substr(uri_end + 1));
				} else {
					std::cerr << "Invalid EXT-X-MAP line in m3u8: " << line << std::endl;
					rewritten_m3u8 += std::string(line) + "\n";
				}

			} else if (line.starts_with("#")) {
				rewritten_m3u8 += std::string(line) + "\n";
			} else {
				rewritten_m3u8 += s3Plugin->genPresignedGetUrl(base_path + std::string(line)) + "\n";
			}
		}

		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setBody(rewritten_m3u8);
		resp->setContentTypeCode(drogon::CT_CUSTOM);
		resp->setContentTypeString("application/vnd.apple.mpegurl");

		co_return resp;
	}
	catch (const drogon::orm::UnexpectedRows& e) {
		// Not found 404
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
		resp->setBody("Video not found");
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "Failed to build play m3u8 response: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to build play m3u8 response: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::incrementVideoViews([[maybe_unused]] HttpRequestPtr req, std::string id) {
	drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
	try {
		auto video = co_await mapper.findByPrimaryKey(id);
		if (*video.getStatus() != (uint8_t)Status::completed) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
			resp->setBody("Video is not available for viewing");
			co_return resp;
		}
		std::string video_id = *video.getVideoId();
		std::string client_ip = req->getHeader("X-Real-IP");
		if (!client_ip.empty()) {
			auto pos = client_ip.find(',');
			if (pos != std::string::npos) client_ip = client_ip.substr(0, pos);
		} else {
			client_ip = req->getPeerAddr().toIp();
		}
		std::string history_key = "viewed:" + video_id + ":" + client_ip;
		std::string counter_key = "pending_views:" + video_id;
		int video_duration = *video.getDuration();

		auto redis = drogon::app().getRedisClient();
		if (!redis) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
			resp->setBody("Failed to get Redis client");
			co_return resp;
		}

		auto result = co_await redis->execCommandCoro("SET %s 1 EX %d NX", history_key.c_str(), video_duration);
		bool is_new_view = !result.isNil();

		if (is_new_view) {
			co_await redis->execCommandCoro("INCR %s", counter_key.c_str());
		}

		Json::Value ret;
		ret["message"] = is_new_view ? "View count incremented" : "View already counted recently";
		ret["counted"] = is_new_view;

		auto resp = drogon::HttpResponse::newHttpJsonResponse(ret);
		resp->setStatusCode(drogon::HttpStatusCode::k200OK);
		co_return resp;

	}
	catch (const drogon::orm::UnexpectedRows& e) {
		// Not found 404
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
		resp->setBody("Video not found");
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "DB Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to increment view count: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::getLikes([[maybe_unused]] HttpRequestPtr req, std::string id) {
	drogon::orm::CoroMapper<drogon_model::playbacq::VideoLikes> mapper(drogon::app().getDbClient());
	try {
		auto likesList = co_await mapper.findBy(drogon::orm::Criteria(drogon_model::playbacq::VideoLikes::Cols::_video_id, drogon::orm::CompareOperator::EQ, id));
		Json::Value jsonResponse(Json::arrayValue);
		for (const auto& like : likesList) {
			jsonResponse.append(*like.getUserId());
		}
		auto resp = drogon::HttpResponse::newHttpJsonResponse(jsonResponse);
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "DB Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to retrieve likes: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::addLike(HttpRequestPtr req, std::string id) {
	std::string userId = req->getAttributes()->get<std::string>("userId");
	try {
		drogon::orm::CoroMapper<drogon_model::playbacq::VideoLikes> mapper(drogon::app().getDbClient());
		// すでにいいねしているか確認
		auto existingLikes = co_await mapper.findBy(drogon::orm::Criteria(drogon_model::playbacq::VideoLikes::Cols::_video_id, drogon::orm::CompareOperator::EQ, id) &&
			drogon::orm::Criteria(drogon_model::playbacq::VideoLikes::Cols::_user_id, drogon::orm::CompareOperator::EQ, userId));
		if (!existingLikes.empty()) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
			resp->setBody("You have already liked this video");
			co_return resp;
		}
		drogon_model::playbacq::VideoLikes newLike;
		newLike.setVideoId(id);
		newLike.setUserId(userId);
		newLike.setCreatedAt(trantor::Date::now());
		co_await mapper.insert(newLike);

		Json::Value jsonResponse;
		jsonResponse["message"] = "Video liked successfully";
		auto resp = drogon::HttpResponse::newHttpJsonResponse(jsonResponse);
		resp->setStatusCode(drogon::HttpStatusCode::k201Created);
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "DB Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to like video: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::removeLike(HttpRequestPtr req, std::string id) {
	std::string userId = req->getAttributes()->get<std::string>("userId");
	try {
		drogon::orm::CoroMapper<drogon_model::playbacq::VideoLikes> mapper(drogon::app().getDbClient());
		auto existingLikes = co_await mapper.findBy(drogon::orm::Criteria(drogon_model::playbacq::VideoLikes::Cols::_video_id, drogon::orm::CompareOperator::EQ, id) &&
			drogon::orm::Criteria(drogon_model::playbacq::VideoLikes::Cols::_user_id, drogon::orm::CompareOperator::EQ, userId));
		if (existingLikes.empty()) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
			resp->setBody("You have not liked this video");
			co_return resp;
		}
		co_await mapper.deleteBy(drogon::orm::Criteria(drogon_model::playbacq::VideoLikes::Cols::_video_id, drogon::orm::CompareOperator::EQ, id) &&
			drogon::orm::Criteria(drogon_model::playbacq::VideoLikes::Cols::_user_id, drogon::orm::CompareOperator::EQ, userId));

		Json::Value jsonResponse;
		jsonResponse["message"] = "Like removed successfully";
		auto resp = drogon::HttpResponse::newHttpJsonResponse(jsonResponse);
		resp->setStatusCode(drogon::HttpStatusCode::k200OK);
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "DB Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to remove like: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::getVideoTopThumbnail([[maybe_unused]] HttpRequestPtr req, std::string id) {
	co_return co_await getVideoThumbnails(req, id, "thumbnail.jpg");
}

drogon::Task<drogon::HttpResponsePtr> videos::getVideoThumbnails([[maybe_unused]] HttpRequestPtr req, std::string id, std::string filename) {
	// サムネイル画像へリダイレクト
	drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
	try {
		// 動画が存在するか確認
		auto video = co_await mapper.findByPrimaryKey(id);

		auto s3Plugin = drogon::app().getPlugin<S3Plugin>();
		std::string presigned_url = s3Plugin->genPresignedGetUrl("hls/" + id + "/" + filename, 60);

		auto resp = drogon::HttpResponse::newRedirectionResponse(presigned_url);
		co_return resp;
	}
	catch (const drogon::orm::UnexpectedRows& e) {
		// Not found 404
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
		resp->setBody("Video not found");
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "Failed to fetch thumbnail from MinIO: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to fetch thumbnail from MinIO: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::buildVideoThumbnailVtt(std::string id, bool isEmbed) {
	// WebVTTファイルを取得
	drogon::orm::CoroMapper<drogon_model::playbacq::Videos> mapper(drogon::app().getDbClient());
	try {
		auto s3Plugin = drogon::app().getPlugin<S3Plugin>();
		std::string webVTT = s3Plugin->getObject("hls/" + id + "/thumbnails.vtt");
		std::string token = Token::generateEmbedToken(id);
		if (isEmbed) {
			const std::string before_str = "api/videos";
			size_t pos = 0;
			while ((pos = webVTT.find(before_str, pos)) != std::string::npos) {
				webVTT.replace(pos, before_str.length(), "unauthApi/embed");
				size_t jpg_pos = webVTT.find(".jpg", pos);
				if (jpg_pos != std::string::npos) {
					size_t insert_pos = jpg_pos + std::string(".jpg").length();
					webVTT.insert(insert_pos, "?token=" + token);
					pos = insert_pos + std::string("?token=").length() + token.length();
				} else {
					pos += std::string("unauthApi/embed").length();
				}
			}
		}
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setBody(webVTT);
		resp->setContentTypeCode(drogon::CT_CUSTOM);
		resp->setContentTypeString("text/vtt");
		co_return resp;
	}
	catch (const drogon::orm::UnexpectedRows& e) {
		// Not found 404
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
		resp->setBody("Video not found");
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "Failed to fetch thumbnail VTT from MinIO: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to fetch thumbnail VTT from MinIO: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::getVideoThumbnailVtt([[maybe_unused]] HttpRequestPtr req, std::string id) {
	co_return co_await buildVideoThumbnailVtt(id, false);
}

drogon::Task<drogon::HttpResponsePtr> videos::getTags([[maybe_unused]] HttpRequestPtr req, std::string video_id) {
	//動画に紐づくタグを取得する
	drogon::orm::CoroMapper<drogon_model::playbacq::VideoTags> mapper(drogon::app().getDbClient());
	try {
		// IDを取得
		auto tagsList = co_await mapper.findBy(drogon::orm::Criteria(drogon_model::playbacq::VideoTags::Cols::_video_id, drogon::orm::CompareOperator::EQ, video_id));
		// IDからタグ名を取得
		Json::Value jsonResponse(Json::arrayValue);
		drogon::orm::CoroMapper<drogon_model::playbacq::Tags> tagMapper(drogon::app().getDbClient());
		for (const auto& videoTag : tagsList) {
			auto tag = co_await tagMapper.findBy(drogon::orm::Criteria(drogon_model::playbacq::Tags::Cols::_tag_id, drogon::orm::CompareOperator::EQ, videoTag.getValueOfTagId()));
			if (!tag.empty()) {
				Json::Value tagJson;
				tagJson["tag_id"] = tag[0].getValueOfTagId();
				tagJson["name"] = tag[0].getValueOfName();
				jsonResponse.append(tagJson);
			}
		}
		auto resp = drogon::HttpResponse::newHttpJsonResponse(jsonResponse);
		co_return  resp;
	}
	catch (const std::exception& e) {
		std::cerr << "DB Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to retrieve tags: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::addTag(HttpRequestPtr req, std::string video_id) {
	auto jsonPtr = req->getJsonObject();
	if (!jsonPtr) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
		resp->setBody("Invalid JSON format");
		co_return resp;
	}
	try {
		std::string tagName = jsonPtr->get("tag", "NULL").asString();
		drogon::orm::CoroMapper<drogon_model::playbacq::Tags> tagMapper(drogon::app().getDbClient());
		auto tags = co_await tagMapper.findBy(drogon::orm::Criteria(drogon_model::playbacq::Tags::Cols::_name, drogon::orm::CompareOperator::EQ, tagName));
		int32_t tagId;
		if (tags.empty()) {
			drogon_model::playbacq::Tags newTag;
			newTag.setName(tagName);
			auto insertedTag = co_await tagMapper.insert(newTag);
			tagId = insertedTag.getValueOfTagId();
		} else {
			tagId = tags[0].getValueOfTagId();
		}
		drogon::orm::CoroMapper<drogon_model::playbacq::VideoTags> videoTagMapper(drogon::app().getDbClient());
		drogon_model::playbacq::VideoTags newVideoTag;
		newVideoTag.setVideoId(video_id);
		newVideoTag.setTagId(tagId);
		co_await videoTagMapper.insert(newVideoTag);

		Json::Value ret;
		ret["tag_id"] = tagId;
		ret["name"] = tagName;
		auto resp = drogon::HttpResponse::newHttpJsonResponse(ret);
		resp->setStatusCode(drogon::HttpStatusCode::k200OK);
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to add tag: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> videos::removeTag(HttpRequestPtr req, std::string video_id) {
	auto jsonPtr = req->getJsonObject();
	if (!jsonPtr) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
		resp->setBody("Invalid JSON format");
		co_return resp;
	}
	try {
		std::string tag_id_str = jsonPtr->get("tag_id", "NULL").asString();
		if (tag_id_str == "NULL") {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
			resp->setBody("tag_id is required");
			co_return resp;
		}
		int32_t tagId;
		try {
			tagId = std::stoi(tag_id_str);
		}
		catch (const std::exception& e) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
			resp->setBody("Invalid tag_id format");
			co_return resp;
		}
		// tag_idからタグが存在するか確認
		drogon::orm::CoroMapper<drogon_model::playbacq::Tags> tagMapper(drogon::app().getDbClient());
		auto tags = co_await tagMapper.findBy(drogon::orm::Criteria(drogon_model::playbacq::Tags::Cols::_tag_id, drogon::orm::CompareOperator::EQ, tagId));
		if (tags.empty()) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
			resp->setBody("Tag not found");
			co_return resp;
		}
		// 動画と紐づかれているか再確認
		drogon::orm::CoroMapper<drogon_model::playbacq::VideoTags> videoTagMapper(drogon::app().getDbClient());
		auto videoTags = co_await videoTagMapper.findBy(
			drogon::orm::Criteria(drogon_model::playbacq::VideoTags::Cols::_video_id, drogon::orm::CompareOperator::EQ, video_id) &&
			drogon::orm::Criteria(drogon_model::playbacq::VideoTags::Cols::_tag_id, drogon::orm::CompareOperator::EQ, tagId)
		);
		if (videoTags.empty()) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
			resp->setBody("Tag not associated with this video");
			co_return resp;
		}
		//問題がなければ関係解消
		co_await videoTagMapper.deleteOne(videoTags[0]);

		Json::Value ret;
		ret["message"] = "Tag removed successfully";
		auto resp = drogon::HttpResponse::newHttpJsonResponse(ret);
		resp->setStatusCode(drogon::HttpStatusCode::k200OK);
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to remove tag: " + std::string(e.what()));
		co_return resp;
	}
}

bool videos::embedAuth(HttpRequestPtr req, std::string id) {
	auto token = req->getOptionalParameter<std::string>("token").value_or("");
	return Token::validateToken(id, token);
}

drogon::Task<drogon::HttpResponsePtr> videos::getM3u8(HttpRequestPtr req, std::string id) {
	// 念のためこっちでも認証の確認
	if (!embedAuth(req, id)) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k403Forbidden);
		resp->setBody("Forbidden");
		co_return resp;
	}
	co_return co_await getVideoPlayM3u8(req, id);
}

drogon::Task<drogon::HttpResponsePtr> videos::getVtt(HttpRequestPtr req, std::string id) {
	// 念のためこっちでも認証の確認
	if (!embedAuth(req, id)) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k403Forbidden);
		resp->setBody("Forbidden");
		co_return resp;
	}
	co_return co_await buildVideoThumbnailVtt(id, true);
}

drogon::Task<drogon::HttpResponsePtr> videos::getThumbnails(HttpRequestPtr req, std::string id, std::string filename) {
	// 念のためこっちでも認証の確認
	if (!embedAuth(req, id)) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k403Forbidden);
		resp->setBody("Forbidden");
		co_return resp;
	}
	co_return co_await getVideoThumbnails(req, id, filename);
}

# include "BotPlugin.h"
# include "traQAPI.h"
# include "YoutubeAPI.h"
# include "models/Comments.h"
# include "controllers/websocket_comments.h"

void BotPlugin::initAndStart(const Json::Value& config)
{
	const char* apiKeyEnv = std::getenv("YOUTUBE_API_KEY");
	if (apiKeyEnv == nullptr) {
		std::cerr << "YOUTUBE_API_KEY is not set. BotPlugin will not be initialized." << std::endl;
		return;
	}
	youtubeApiKey = std::string(apiKeyEnv);
}

std::optional<std::chrono::sys_time<std::chrono::milliseconds>> BotPlugin::iso8601ToSysTime(const std::string& iso8601) {
	std::istringstream ss(iso8601);
	std::chrono::sys_time<std::chrono::milliseconds> tp;
	ss >> std::chrono::parse("%FT%T", tp);
	if (ss.fail()) {
		return std::nullopt;
	}
	return tp;
}

drogon::Task<void> BotPlugin::registerChannel(
	const std::string& channelId,
	const std::string& videoId,
	double delay
) {
	// チャンネル存在チェック
	auto* traqAPI = drogon::app().getPlugin<traQAPI>();
	if (!traqAPI) {
		std::cerr << "traQAPI plugin is not initialized" << std::endl;
		co_return;
	}
	auto channelExists = co_await traqAPI->channelExists(channelId);
	if (!channelExists) {
		std::cerr << "Channel does not exist: " << channelId << std::endl;
		co_return;
	}
	// チャンネルがすでに登録されている場合は前のライブが終了しているかを確認する
	if (liveVideos.contains(channelId)) {
		const std::string youtubeID = liveVideos[channelId].videoId;
		auto videoInfoRaw = co_await YoutubeAPI::fetchLiveInfo(youtubeID, youtubeApiKey);
		if (!videoInfoRaw) {
			std::cerr << "Failed to fetch live info from YouTube API for channel: " << channelId << std::endl;
			co_return;
		}
		const YoutubeAPI::LiveInfo& videoInfo = videoInfoRaw.value();
		if (!videoInfo.actualEndTime.has_value()) {
			std::cerr << "Channel already registered and live stream has not ended: " << channelId << std::endl;
			co_return;
		}
		// ライブが終了している場合はクリーンアップ
		liveVideos.erase(channelId);
	} else {
		// チャンネルに参加する
		try {
			co_await traqAPI->joinChannel(channelId);
		}
		catch (const std::exception& e) {
			std::cerr << "Failed to join channel: " << channelId << ". Error: " << e.what() << std::endl;
			co_return;
		}
	}

	liveVideos[channelId] = { videoId, delay };
	co_return;
}

drogon::Task<drogon::HttpResponsePtr> BotPlugin::postComment(
	std::string_view channelId,
	std::string_view comment,
	std::string_view createdAt,
	std::string_view messageId
) {
	if (!liveVideos.contains(channelId.data())) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
		resp->setBody("Channel not registered for live video");
		co_return resp;
	}
	std::string badRequestMessage;
	if (comment.empty()) badRequestMessage = "Comment is empty";
	if (createdAt.empty()) badRequestMessage = "CreatedAt is empty";
	if (messageId.empty()) badRequestMessage = "MessageId is empty";
	if (!badRequestMessage.empty()) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
		resp->setBody(badRequestMessage);
		co_return resp;
	}
	const std::string youtubeID = liveVideos[channelId.data()].videoId;
	if (!liveVideos[channelId.data()].actualStartTime.has_value()) {
		// ライブ配信が始まったかどうか確認
		auto videoInfoRaw = co_await YoutubeAPI::fetchLiveInfo(youtubeID, youtubeApiKey);
		if (!videoInfoRaw) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
			resp->setBody("Failed to fetch live info from YouTube API");
			co_return resp;
		}
		const YoutubeAPI::LiveInfo& videoInfo = videoInfoRaw.value();
		// 配信が始まっていない場合は早期リターン
		if (!videoInfo.actualStartTime.has_value()) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
			resp->setBody("Live stream has not started yet");
			co_return resp;
		}
		// 配信が始まった場合はactualStartTimeを保存
		if (!liveVideos[channelId.data()].actualStartTime.has_value())
			liveVideos[channelId.data()].actualStartTime = iso8601ToSysTime(*videoInfo.actualStartTime);
	}
	// 配信が終了したかを確認
	{
		auto* loop = drogon::app().getLoop();
		std::lock_guard<std::mutex> lockGuard(mutex_);
		// タイマーをリセット
		if (check_timers_.contains(channelId.data()))
			loop->invalidateTimer(check_timers_[channelId.data()]);
		// 1分後に終了確認するタイマーをセット
		trantor::TimerId timer_id = loop->runAfter(60.0, [this, channelId = std::string(channelId), youtubeID]() {
			drogon::async_run([this, channelId, youtubeID]() -> drogon::Task<void> {
				auto videoInfoRaw = co_await YoutubeAPI::fetchLiveInfo(youtubeID, youtubeApiKey);
				if (videoInfoRaw && videoInfoRaw->actualEndTime.has_value()) {
					// ライブが終了している場合はクリーンアップ
					std::lock_guard<std::mutex> lockGuard(mutex_);
					liveVideos.erase(channelId);
					check_timers_.erase(channelId);
					// チャンネルから退出する
					auto* traqAPI = drogon::app().getPlugin<traQAPI>();
					if (traqAPI) {
						try {
							co_await traqAPI->leaveChannel(channelId);
						}
						catch (const std::exception& e) {
							std::cerr << "Failed to leave channel: " << channelId << ". Error: " << e.what() << std::endl;
						}
					} else {
						std::cerr << "traQAPI plugin is not initialized" << std::endl;
					}
				}
				co_return;
				});
			});
		check_timers_[channelId.data()] = timer_id;
	}

	auto msgTime = iso8601ToSysTime(createdAt.data());
	if (!msgTime.has_value()) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
		resp->setBody("Invalid CreatedAt format");
		co_return resp;
	}

	try {
		drogon_model::playbacq::Comments newComment;
		newComment.setVideoId(liveVideos[channelId.data()].videoId);
		newComment.setComment(comment.data());
		newComment.setUserId("playbacQ Bot");
		newComment.setCreatedAt(trantor::Date::now());
		auto playTime = msgTime.value() - liveVideos[channelId.data()].actualStartTime.value();
		double timestamp = std::chrono::duration<double>(playTime).count() / 1000.0 + liveVideos[channelId.data()].delay;
		newComment.setTimestamp(timestamp);
		newComment.setCommand("");

		drogon::orm::CoroMapper<drogon_model::playbacq::Comments> mapper(drogon::app().getDbClient());
		auto insertedComment = co_await mapper.insert(newComment);
		// Websocketでリアルタイムにコメントを配信
		CommentController::broadcastToRoom(liveVideos[channelId.data()].videoId, insertedComment.toJson().toStyledString());
		// コメントIDを保存
		liveVideos[channelId.data()].comments[messageId.data()] = *insertedComment.getCommentId();
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k201Created);
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to create comment: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> BotPlugin::updateComment(
	std::string_view channelId,
	std::string_view comment,
	std::string_view messageId
) {
	std::string notFoundMessage;
	if (!liveVideos.contains(channelId.data())) notFoundMessage = "Channel not registered for live video";
	if (!liveVideos[channelId.data()].comments.contains(messageId.data()))
		notFoundMessage = "Comment not found for this channel";
	if (!notFoundMessage.empty()) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
		resp->setBody(notFoundMessage);
		co_return resp;
	}

	int32_t commentId = liveVideos[channelId.data()].comments[messageId.data()];
	try {
		drogon::orm::CoroMapper<drogon_model::playbacq::Comments> mapper(drogon::app().getDbClient());
		auto comments = co_await mapper.findByPrimaryKey(commentId);
		comments.setComment(comment.data());
		size_t updatedCount = co_await mapper.update(comments);
		if (updatedCount == 0) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
			resp->setBody("Failed to update comment");
			co_return resp;
		}
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k200OK);
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to update comment: " + std::string(e.what()));
		co_return resp;
	}
}

drogon::Task<drogon::HttpResponsePtr> BotPlugin::deleteComment(
	std::string_view channelId,
	std::string_view messageId
) {
	std::string notFoundMessage;

	if (!liveVideos.contains(channelId.data())) notFoundMessage = "Channel not registered for live video";
	if (!liveVideos[channelId.data()].comments.contains(messageId.data())) notFoundMessage = "Comment not found for this channel";
	if (!notFoundMessage.empty()) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
		resp->setBody(notFoundMessage);
		co_return resp;
	}
	int32_t commentId = liveVideos[channelId.data()].comments[messageId.data()];
	try {
		drogon::orm::CoroMapper<drogon_model::playbacq::Comments> mapper(drogon::app().getDbClient());
		size_t deletedCount = co_await mapper.deleteByPrimaryKey(commentId);
		if (deletedCount == 0) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
			resp->setBody("Failed to delete comment");
			co_return resp;
		}
		liveVideos[channelId.data()].comments.erase(messageId.data());
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k200OK);
		co_return resp;
	}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
		resp->setBody("Failed to delete comment: " + std::string(e.what()));
		co_return resp;
	}
}

void BotPlugin::shutdown()
{
	/// Shutdown the plugin
}

# include "BotPlugin.h"
# include "traQAPI.h"
# include "YoutubeAPI.h"
# include "models/Videos.h"
# include "models/Comments.h"
# include "models/LiveVideos.h"
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
	drogon::orm::CoroMapper<drogon_model::playbacq::LiveVideos> mapper(drogon::app().getDbClient());
	try {
		auto liveVideo = co_await mapper.findBy(
			drogon::orm::Criteria(drogon_model::playbacq::LiveVideos::Cols::_channel_id, drogon::orm::CompareOperator::EQ, channelId)
		);
		// チャンネルがすでに登録されている場合は前のライブが終了しているかを確認する
		if (!liveVideo.empty()) {
			if (liveVideo.size() > 1) {
				std::cerr << "[FATAL] Multiple live video entries found for channel: " << channelId << std::endl;
				co_return;
			}
			const auto& existingLiveVideo = liveVideo.front();
			const std::string youtubeID = existingLiveVideo.getValueOfVideoId();
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
			co_await mapper.deleteByPrimaryKey(existingLiveVideo.getPrimaryKey());
			std::lock_guard<std::mutex> lockGuard(mutex_);
			if (check_timers_.contains(channelId))
				check_timers_.erase(channelId);
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
		// DBに登録
		drogon_model::playbacq::LiveVideos newLiveVideo;
		newLiveVideo.setVideoId(videoId);
		newLiveVideo.setChannelId(channelId);
		newLiveVideo.setDelay(delay);
		co_await mapper.insert(newLiveVideo);
	}
	catch (const std::exception& e) {
		std::cerr << "Failed to register channel: " << channelId << ". Error: " << e.what() << std::endl;
	}
	co_return;
}

drogon::Task<drogon::HttpResponsePtr> BotPlugin::postComment(
	std::string_view channelId,
	std::string_view comment,
	std::string_view createdAt,
	std::string_view messageId
) {
	drogon::orm::CoroMapper<drogon_model::playbacq::LiveVideos> mapper(drogon::app().getDbClient());
	try {
		auto liveVideo = co_await mapper.findBy(
			drogon::orm::Criteria(drogon_model::playbacq::LiveVideos::Cols::_channel_id, drogon::orm::CompareOperator::EQ, channelId.data())
		);
		if (liveVideo.empty()) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
			resp->setBody("Channel not registered for live video");
			co_return resp;
		} else if (liveVideo.size() > 1) {
			std::cerr << "[FATAL] Multiple live video entries found for channel: " << channelId << std::endl;
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
			resp->setBody("Multiple live video entries found for channel");
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
		auto& liveVideoEntry = liveVideo.front();
		const std::string youtubeID = liveVideoEntry.getValueOfVideoId();
		if (!liveVideoEntry.getActualStartTime()) {
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
			auto actualStartTime = iso8601ToSysTime(*videoInfo.actualStartTime);
			if (!actualStartTime.has_value()) {
				auto resp = drogon::HttpResponse::newHttpResponse();
				resp->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
				resp->setBody("Invalid actualStartTime format from YouTube API");
				co_return resp;
			}
			liveVideoEntry.setActualStartTime(
				trantor::Date(
					std::chrono::duration_cast<std::chrono::microseconds>(
						actualStartTime.value().time_since_epoch()
					).count()
				));
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
						drogon::orm::CoroMapper<drogon_model::playbacq::LiveVideos> mapper(drogon::app().getDbClient());
						try {
							auto liveVideo = co_await mapper.findByPrimaryKey(make_tuple(youtubeID, channelId));
							co_await mapper.deleteByPrimaryKey(liveVideo.getPrimaryKey());
						}
						catch (const std::exception& e) {
							std::cerr << "Failed to find live video for cleanup: " << e.what() << std::endl;
							co_return;
						}
						std::lock_guard<std::mutex> lockGuard(mutex_);
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
		// playbacQのvideo IDを取得
		drogon::orm::CoroMapper<drogon_model::playbacq::Videos> videoMapper(drogon::app().getDbClient());
		auto videos = co_await videoMapper.findBy(
			drogon::orm::Criteria(drogon_model::playbacq::Videos::Cols::_video_url, drogon::orm::CompareOperator::EQ, youtubeID)
		);
		if (videos.empty()) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
			resp->setBody("Video not found");
			co_return resp;
		}
		if (videos.size() > 1) {
			std::cerr << "[FATAL] Multiple video entries found for YouTube ID: " << youtubeID << std::endl;
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
			resp->setBody("Multiple video entries found for YouTube ID");
			co_return resp;
		}
		auto videoId = videos.front().getValueOfVideoId();
		// コメントをDBに保存
		drogon_model::playbacq::Comments newComment;
		newComment.setVideoId(videoId);
		newComment.setComment(comment.data());
		newComment.setUserId("playbacQ Bot");
		newComment.setCreatedAt(trantor::Date::now());
		newComment.setTraqMessageId(messageId.data());
		auto playTime = msgTime.value() -
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::microseconds(
					liveVideoEntry.getValueOfActualStartTime().microSecondsSinceEpoch()
				)
			);
		double timestamp = playTime.time_since_epoch().count() / 1000.0 + liveVideoEntry.getValueOfDelay();
		newComment.setTimestamp(timestamp);
		newComment.setCommand("");

		drogon::orm::CoroMapper<drogon_model::playbacq::Comments> mapper(drogon::app().getDbClient());
		auto insertedComment = co_await mapper.insert(newComment);
		// Websocketでリアルタイムにコメントを配信
		CommentController::broadcastToRoom(videoId, insertedComment.toJson().toStyledString());
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
	std::string_view comment,
	std::string_view messageId
) {
	drogon::orm::CoroMapper<drogon_model::playbacq::Comments> mapper(drogon::app().getDbClient());
	try {
		auto comments = co_await mapper.findBy(
			drogon::orm::Criteria(drogon_model::playbacq::Comments::Cols::_traq_message_id, drogon::orm::CompareOperator::EQ, messageId.data())
		);
		if (comments.empty()) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
			resp->setBody("Comment not found");
			co_return resp;
		}
		if (comments.size() > 1) {
			std::cerr << "[FATAL] Multiple comment entries found for message: " << messageId << std::endl;
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
			resp->setBody("Multiple comment entries found for message: " + std::string(messageId.data()));
			co_return resp;
		}
		auto& commentEntry = comments.front();
		commentEntry.setComment(comment.data());
		size_t updatedCount = co_await mapper.update(commentEntry);
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
	std::string_view messageId
) {
	drogon::orm::CoroMapper<drogon_model::playbacq::Comments> mapper(drogon::app().getDbClient());
	try {
		auto comments = co_await mapper.findBy(
			drogon::orm::Criteria(drogon_model::playbacq::Comments::Cols::_traq_message_id, drogon::orm::CompareOperator::EQ, messageId.data())
		);
		if (comments.empty()) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k404NotFound);
			resp->setBody("Comment not found");
			co_return resp;
		}
		if (comments.size() > 1) {
			std::cerr << "[FATAL] Multiple comment entries found for message: " << messageId << std::endl;
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
			resp->setBody("Multiple comment entries found for message: " + std::string(messageId.data()));
			co_return resp;
		}
		auto& commentEntry = comments.front();
		size_t deletedCount = co_await mapper.deleteByPrimaryKey(commentEntry.getPrimaryKey());
		if (deletedCount == 0) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setStatusCode(drogon::HttpStatusCode::k500InternalServerError);
			resp->setBody("Failed to delete comment");
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
		resp->setBody("Failed to delete comment: " + std::string(e.what()));
		co_return resp;
	}
}

void BotPlugin::shutdown()
{
	/// Shutdown the plugin
}

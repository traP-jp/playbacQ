#pragma once

#include <drogon/utils/coroutine.h>
#include <drogon/HttpController.h>
#include <drogon/HttpClient.h>

using namespace drogon;

namespace api
{
  class videos : public drogon::HttpController<videos>
  {
  private:
    bool embedAuth(HttpRequestPtr req, std::string id);
    drogon::Task<drogon::HttpResponsePtr> buildVideoThumbnailVtt(std::string id, bool isEmbed);
  public:
    METHOD_LIST_BEGIN;
    ADD_METHOD_TO(videos::getVideos, "/api/videos", Get);
    ADD_METHOD_TO(videos::postVideos, "/api/videos", Post, "AuthFilter");
    ADD_METHOD_TO(videos::deleteVideo, "/api/videos", Delete, "AuthFilter");
    ADD_METHOD_TO(videos::postExVideo, "/api/ex-videos", Post, "AuthFilter");

    ADD_METHOD_TO(videos::getVideo, "/api/videos/{1}", Get);
    ADD_METHOD_TO(videos::patchVideo, "/api/videos/{1}", Patch, "AuthFilter");

    ADD_METHOD_TO(videos::getVideoProgress, "/api/videos/{1}/progress", Get);
    ADD_METHOD_TO(videos::getVideoPlayM3u8, "/api/videos/{1}/play", Get);
    ADD_METHOD_TO(videos::incrementVideoViews, "/api/videos/{1}/views", Post);

    ADD_METHOD_TO(videos::getLikes, "/api/videos/{1}/likes", Get);
    ADD_METHOD_TO(videos::addLike, "/api/videos/{1}/likes", Post, "AuthFilter");
    ADD_METHOD_TO(videos::removeLike, "/api/videos/{1}/likes", Delete, "AuthFilter");

    ADD_METHOD_TO(videos::getVideoTopThumbnail, "/api/videos/{1}/thumbnail", Get);
    ADD_METHOD_TO(videos::getVideoThumbnails, "/api/videos/{1}/thumbnails/{2}", Get);
    ADD_METHOD_TO(videos::getVideoThumbnailVtt, "/api/videos/{1}/vtt", Get);

    ADD_METHOD_TO(videos::getTags, "/api/videos/{1}/tags", Get);
    ADD_METHOD_TO(videos::addTag, "/api/videos/{1}/tags", Post);
    ADD_METHOD_TO(videos::removeTag, "/api/videos/{1}/tags", Delete);

    ADD_METHOD_TO(videos::getM3u8, "/unauthApi/embed/{1}", Get);
    ADD_METHOD_TO(videos::getVtt, "/unauthApi/embed/{1}/vtt", Get);
    ADD_METHOD_TO(videos::getThumbnails, "/unauthApi/embed/{1}/thumbnails/{2}", Get);
    METHOD_LIST_END;
    drogon::Task<drogon::HttpResponsePtr> getVideos(HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> postVideos(HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> deleteVideo(HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> postExVideo(HttpRequestPtr req);

    drogon::Task<drogon::HttpResponsePtr> getVideo(HttpRequestPtr req, std::string id);
    drogon::Task<drogon::HttpResponsePtr> patchVideo(HttpRequestPtr req, std::string id);

    drogon::Task<drogon::HttpResponsePtr> getVideoProgress(HttpRequestPtr req, std::string id);
    drogon::Task<drogon::HttpResponsePtr> getVideoPlayM3u8(HttpRequestPtr req, std::string id);
    drogon::Task<drogon::HttpResponsePtr> incrementVideoViews(HttpRequestPtr req, std::string id);

    drogon::Task<drogon::HttpResponsePtr> getLikes(HttpRequestPtr req, std::string id);
    drogon::Task<drogon::HttpResponsePtr> addLike(HttpRequestPtr req, std::string id);
    drogon::Task<drogon::HttpResponsePtr> removeLike(HttpRequestPtr req, std::string id);

    drogon::Task<drogon::HttpResponsePtr> getVideoTopThumbnail(HttpRequestPtr req, std::string id);
    drogon::Task<drogon::HttpResponsePtr> getVideoThumbnails(HttpRequestPtr req, std::string id, std::string filename);
    drogon::Task<drogon::HttpResponsePtr> getVideoThumbnailVtt(HttpRequestPtr req, std::string id);

    drogon::Task<drogon::HttpResponsePtr> getTags(HttpRequestPtr req, std::string id);
    drogon::Task<drogon::HttpResponsePtr> addTag(HttpRequestPtr req, std::string id);
    drogon::Task<drogon::HttpResponsePtr> removeTag(HttpRequestPtr req, std::string id);

    drogon::Task<drogon::HttpResponsePtr> getM3u8(HttpRequestPtr req, std::string id);
    drogon::Task<drogon::HttpResponsePtr> getVtt(HttpRequestPtr req, std::string id);
    drogon::Task<drogon::HttpResponsePtr> getThumbnails(HttpRequestPtr req, std::string id, std::string filename);
  };
}

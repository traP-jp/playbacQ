#include <iostream>
#include <string>
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <sw/redis++/redis++.h>
#include <boost/process.hpp>
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/core/auth/AWSCredentials.h>
#include <curl/curl.h>

namespace {
#ifdef USE_NVIDIA_ENCODER
constexpr bool USE_NVIDIA_VIDEO_ENCODER = true;
constexpr const char* VIDEO_ENCODER = "h264_nvenc";
constexpr const char* VIDEO_PRESET = "p4";
constexpr const char* VIDEO_BITRATE = "2M";
#else
constexpr bool USE_NVIDIA_VIDEO_ENCODER = false;
constexpr const char* VIDEO_ENCODER = "libx264";
constexpr const char* VIDEO_PRESET = "veryfast";
constexpr const char* VIDEO_BITRATE = "";
#endif
}

bool upload2MinIO(const std::string& local_file_path, const std::string& bucket_name, const std::string& object_key) {
	const char* envUser = std::getenv("MINIO_ROOT_USER");
	const char* envPassword = std::getenv("MINIO_ROOT_PASSWORD");
#ifdef USE_INTERNAL_S3
	const char* envEndpoint = std::getenv("MINIO_ENDPOINT");
#else
	const char* envEndpoint = std::getenv("S3_ENDPOINT");
#endif
	const std::string accessKey = envUser ? envUser : "";
	const std::string secretKey = envPassword ? envPassword : "";
#ifdef USE_INTERNAL_S3
	const std::string minioEndpoint = envEndpoint ? "http://" + std::string(envEndpoint) : "http://minio:9000";
#else
	const std::string minioEndpoint = envEndpoint ? envEndpoint : "http://127.0.0.1:9000";
#endif
	Aws::Auth::AWSCredentials credentials(accessKey.c_str(), secretKey.c_str());
	Aws::Client::ClientConfiguration clientConfig;
	clientConfig.endpointOverride = minioEndpoint;
	clientConfig.region = "us-east-1";
#ifdef USE_INTERNAL_S3
	clientConfig.scheme = Aws::Http::Scheme::HTTP;
#else
	clientConfig.scheme = Aws::Http::Scheme::HTTPS;
#endif
	Aws::S3::S3Client s3_client(credentials, clientConfig, Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never, false);

	Aws::S3::Model::PutObjectRequest request;
	request.SetBucket(bucket_name);
	request.SetKey(object_key);

	std::shared_ptr<Aws::IOStream> input_data =
		Aws::MakeShared<Aws::FStream>("PutObjectInputStream", local_file_path.c_str(), std::ios_base::in | std::ios_base::binary);

	if (!input_data->good()) {
		std::cerr << "Failed to open local file: " << local_file_path << std::endl;
		return false;
	}
	request.SetBody(input_data);

	auto outcome = s3_client.PutObject(request);
	if (outcome.IsSuccess()) {
		std::cout << "Successfully uploaded: " << object_key << std::endl;
		return true;
	} else {
		std::cerr << "Upload failed: " << outcome.GetError().GetMessage() << std::endl;
		return false;
	}
}

void postEncodeResult(const std::string& videoId, const std::string& status, const std::string& message, const int duration = 0) {
	try {
		std::string payload = "{\"video_id\": \"" + videoId + "\", \"status\": \"" + status + "\", \"message\": \"" + message + "\", \"duration\": " + std::to_string(duration) + "}";
		CURL* curl = curl_easy_init();
		if (curl) {
			struct curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
			const char* backendUrlEnv = std::getenv("BACKEND_URL");
			std::string backendUrl = backendUrlEnv ? backendUrlEnv : "http://backend:8080";
			std::cout << "DEBUG: backendUrl is [" << backendUrl << "]" << std::endl;
			std::string fullUrl = backendUrl + "/webhooks/encode_result";
			curl_easy_setopt(curl, CURLOPT_URL, fullUrl.c_str());
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
			std::string response_string;

			// 取得したデータをstringに追記するコールバック関数
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
				size_t total_size = size * nmemb;
				auto* response = static_cast<std::string*>(userdata);
				response->append(static_cast<char*>(ptr), total_size);
				return total_size;
				});
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

			CURLcode res = curl_easy_perform(curl);

			if (res != CURLE_OK) {
				std::cerr << "Webhook failed: " << curl_easy_strerror(res) << std::endl;
			} else {
				long http_code = 0;
				curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

				if (http_code == 200) {
					std::cout << "Sent webhook to Drogon. Status: " << status
						<< " (Backend Response: " << response_string << ")" << std::endl;
				} else {
					std::cerr << "Webhook HTTP Error! HTTP Code: " << http_code
						<< ", URL: " << fullUrl
						<< ", Response: " << response_string << std::endl;
				}
			}

			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
		} else {
			std::cerr << "Failed to initialize CURL" << std::endl;
			return;
		}
	}
	catch (const sw::redis::Error& e) {
		std::cerr << "Redis publish error: " << e.what() << std::endl;
	}
}

bool deleteFromMinIO(const std::string& bucket_name, const std::string& object_key) {
	const char* envUser = std::getenv("MINIO_ROOT_USER");
	const char* envPassword = std::getenv("MINIO_ROOT_PASSWORD");
#ifdef USE_INTERNAL_S3
	const char* envEndpoint = std::getenv("MINIO_ENDPOINT");
#else
	const char* envEndpoint = std::getenv("S3_ENDPOINT");
#endif
	const std::string accessKey = envUser ? envUser : "";
	const std::string secretKey = envPassword ? envPassword : "";
#ifdef USE_INTERNAL_S3
	const std::string minioEndpoint = envEndpoint ? "http://" + std::string(envEndpoint) : "http://minio:9000";
#else
	const std::string minioEndpoint = envEndpoint ? envEndpoint : "http://127.0.0.1:9000";
#endif
	Aws::Auth::AWSCredentials credentials(accessKey.c_str(), secretKey.c_str());
	Aws::Client::ClientConfiguration clientConfig;
	clientConfig.endpointOverride = minioEndpoint;
	clientConfig.region = "us-east-1";
#ifdef USE_INTERNAL_S3
	clientConfig.scheme = Aws::Http::Scheme::HTTP;
#else
	clientConfig.scheme = Aws::Http::Scheme::HTTPS;
#endif
	Aws::S3::S3Client s3_client(credentials, clientConfig, Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never, false);

	Aws::S3::Model::DeleteObjectRequest request;
	request.SetBucket(bucket_name);
	request.SetKey(object_key);

	auto outcome = s3_client.DeleteObject(request);
	if (outcome.IsSuccess()) {
		std::cout << "Successfully deleted original file: " << object_key << std::endl;
		return true;
	} else {
		std::cerr << "Delete failed: " << outcome.GetError().GetMessage() << std::endl;
		return false;
	}
}

std::string formatTime(int total_seconds) {
	int hours = total_seconds / 3600;
	int minutes = (total_seconds % 3600) / 60;
	int seconds = total_seconds % 60;
	return std::format("{:02}:{:02}:{:02}.000", hours, minutes, seconds);
}

int main(int argc, char* argv[]) {
	if (argc != 2) {
		std::cerr << "Usage: " << argv[0] << " <video_id>|--show-encoder-config" << std::endl;
		return 1;
	}

	std::string video_id = argv[1];
	std::cout << "FFmpeg video encoder: " << VIDEO_ENCODER
		<< ", preset: " << VIDEO_PRESET;
	if (VIDEO_BITRATE[0] != '\0') {
		std::cout << ", bitrate: " << VIDEO_BITRATE;
	}
	std::cout << std::endl;
	if (video_id == "--show-encoder-config") {
		return 0;
	}

	Aws::SDKOptions options;
	Aws::InitAPI(options);
	curl_global_init(CURL_GLOBAL_ALL);
	{
		std::cout << "Starting worker..." << std::endl;
		try {
			// ※ホスト名はdocker-composeのサービス名(redis)を指定
			const char* envHost = std::getenv("REDIS_HOST");
			const char* envPort = std::getenv("REDIS_PORT");
			const char* envPass = std::getenv("REDIS_PASSWORD");
			const char* envUser = std::getenv("REDIS_USER");
			std::string redisHost = envHost ? envHost : "redis";
			std::string redisPort = envPort ? envPort : "6379";
			std::string redisPass = envPass ? envPass : "";
			std::string redisUser = envUser ? envUser : "default";
			sw::redis::ConnectionOptions connection_options;
			connection_options.host = redisHost;
			connection_options.port = std::stoi(redisPort);
			connection_options.password = redisPass;
			connection_options.user = redisUser;
			connection_options.tls.enabled = false;

			auto redis = sw::redis::Redis(connection_options);
			std::cout << "Connected to Redis successfully." << std::endl;

#ifdef USE_INTERNAL_S3
			const char* envEndpoint = std::getenv("MINIO_ENDPOINT");
			std::string minioEndpoint = envEndpoint ? "http://" + std::string(envEndpoint) : "http://minio:9000";
#else
			const char* envEndpoint = std::getenv("S3_ENDPOINT");
			std::string minioEndpoint = envEndpoint ? envEndpoint : "http://127.0.0.1:9000";
#endif
			std::cout << "Using MinIO endpoint: " << minioEndpoint << std::endl;

			std::cout << "\n[JOB RECEIVED] Video ID: " << video_id << std::endl;
			int last_attempted_percent = -1;
			auto updateProgress = [&](int progress) {
				progress = std::clamp(progress, 0, 100);
				if (progress <= last_attempted_percent) {
					return;
				}
				last_attempted_percent = progress;
				try {
					redis.set("video:progress:" + video_id, std::to_string(progress), std::chrono::hours(24));
					std::cout << "Progress updated: " << progress << "% for video ID: " << video_id << std::endl;
				}
				catch (const std::exception& e) {
					// 進捗通知の失敗だけでエンコード処理を失敗させない。
					std::cerr << "Progress update failed for video ID " << video_id
						<< ": " << e.what() << std::endl;
				}
				catch (...) {
					std::cerr << "Progress update failed for video ID " << video_id
						<< ": unknown error" << std::endl;
				}
			};
			updateProgress(0);

			const char* envMinIOUser = std::getenv("MINIO_ROOT_USER");
			const char* envMinIOPassword = std::getenv("MINIO_ROOT_PASSWORD");
			std::string accessKey = envMinIOUser ? envMinIOUser : "";
			std::string secretKey = envMinIOPassword ? envMinIOPassword : "";

			Aws::Auth::AWSCredentials credentials(accessKey.c_str(), secretKey.c_str());
			Aws::Client::ClientConfiguration clientConfig;
			clientConfig.endpointOverride = minioEndpoint;
			clientConfig.region = "us-east-1";
#ifdef USE_INTERNAL_S3
			clientConfig.scheme = Aws::Http::Scheme::HTTP;
#else
			clientConfig.scheme = Aws::Http::Scheme::HTTPS;
#endif
			Aws::S3::S3Client s3_client(credentials, clientConfig, Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never, false);

			// 有効期限1時間のダウンロード用URLを発行
			Aws::String presignedUrl = s3_client.GeneratePresignedUrl(
				"videofiles",
				video_id + ".mp4",
				Aws::Http::HttpMethod::HTTP_GET,
				3600
			);
			std::string video_url = presignedUrl.c_str();

			// 動画の総時間をffprobeで取得
			double total_duration_sec = 0.0;
			try {
				boost::process::ipstream probe_is;
				boost::process::child probe_c(boost::process::search_path("ffprobe"),
					boost::process::args({ "-v", "error", "-show_entries", "format=duration", "-of", "default=noprint_wrappers=1:nokey=1", video_url }),
					boost::process::std_out > probe_is
				);

				std::string duration_str;
				if (std::getline(probe_is, duration_str)) {
					total_duration_sec = std::stod(duration_str);
				}
				probe_c.wait();
				if (probe_c.exit_code() != 0) {
					throw std::runtime_error("ffprobe exited with code " + std::to_string(probe_c.exit_code()));
				}

				if (total_duration_sec <= 0.0) {
					std::cerr << "Invalid video duration: " << total_duration_sec << " seconds for video ID: " << video_id << std::endl;
					postEncodeResult(video_id, "failed", "Invalid video duration");
					return 1;
				}
			}
			catch (const std::exception& e) {
				std::cerr << "ffprobe error: " << e.what() << std::endl;
				postEncodeResult(video_id, "failed", "ffprobe error: " + std::string(e.what()));
				return 1;
			}

			std::string base_dir = "/tmp/playbacq_encode/" + video_id + "/";
			int interval = 10; // サムネイルを10秒ごとに生成
			if (total_duration_sec < 600) {
				interval = 2; // 10分未満の動画は2秒ごとにサムネイルを生成
			} else if (total_duration_sec < 3600) {
				interval = 5; // 1時間未満の動画は5秒ごとにサムネイルを生成
			}
			// エンコード処理
			try {
				auto ffmpeg_path = boost::process::search_path("ffmpeg");
				if (ffmpeg_path.empty()) {
					throw std::runtime_error("ffmpeg not found in PATH");
				}
				std::filesystem::create_directories(base_dir);

				boost::process::ipstream output_stream;
				const int thumb_time = std::min(4, static_cast<int>(total_duration_sec / 2));
				const std::string filter_complex = std::format(
					"[0:v]split=4[v_hls_in][v_seek_in][v_thumb_in][v_progress_in];"
					"[v_hls_in]scale='trunc(min(1920,iw)/2)*2':'trunc(min(1080,ih)/2)*2':force_original_aspect_ratio=decrease,pad='ceil(max(iw,ih*(16/9))/2)*2':'ceil(max(ih,iw*(9/16))/2)*2':(ow-iw)/2:(oh-ih)/2:black,format=yuv420p[v_hls_out];"
					"[v_seek_in]fps=1/{0},scale=160:90:force_original_aspect_ratio=decrease,pad=160:90:(ow-iw)/2:(oh-ih)/2:black,tile=10x10[v_seek_out];"
					"[v_thumb_in]select='gte(t\\,{1})',scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:black[v_thumb_out];"
					"[v_progress_in]fps=1,scale=2:2,metadata=mode=add:key=playbacq_progress:value=1,"
					"metadata=mode=print:key=playbacq_progress:file=/dev/stdout:direct=1[v_progress_out]",
					interval,
					thumb_time
				);
				std::vector<std::string> args;
				if (USE_NVIDIA_VIDEO_ENCODER) {
					args.insert(args.end(), { "-hwaccel", "cuda" });
				}
				args.insert(args.end(), {
					"-reconnect", "1",
					"-reconnect_at_eof", "1",
					"-reconnect_streamed", "1",
					"-reconnect_delay_max", "5",
					"-i", video_url,
					"-filter_complex", filter_complex,
					// HLS出力設定
					"-map", "[v_hls_out]",
					"-map", "0:a?",
					"-c:v", VIDEO_ENCODER,
					"-preset", VIDEO_PRESET
				});
				if (VIDEO_BITRATE[0] != '\0') {
					args.insert(args.end(), { "-b:v", VIDEO_BITRATE });
				}
				args.insert(args.end(), {
					"-c:a", "aac",
					"-g", "60",
					"-sc_threshold", "0",
					"-f", "hls",
					"-hls_time", "2",	// セグメントの長さを2秒に設定
					"-hls_segment_type", "fmp4",
					"-hls_flags", "single_file",
					// ---ストリーミング再生ならここまでで良い。---
					"-hls_playlist_type", "vod",
					"-hls_list_size", "0",
					base_dir + "output.m3u8",
					// シークバー用のサムネイル出力設定
					"-map", "[v_seek_out]",
					"-c:v", "mjpeg",
					"-q:v", "2",
					base_dir + "thumbnail%03d.jpg",
					// サムネ画像の出力設定
					"-map", "[v_thumb_out]",
					"-frames:v", "1",
					"-c:v", "mjpeg",
					"-q:v", "2",
					base_dir + "thumbnail.jpg",
					// 1秒ごとの進捗時刻を生成する軽量なnull出力
					"-map", "[v_progress_out]",
					"-f", "null",
					"/dev/null"
				});
				std::cout << "Starting ffmpeg process for video ID: " << video_id << std::endl;
				boost::process::ipstream error_stream;
				boost::process::child ffmpeg_process(ffmpeg_path,
					boost::process::args(args),
					boost::process::std_in.close(),
					boost::process::std_out > output_stream,
					boost::process::std_err > error_stream);
				std::thread error_reader([&error_stream]() {
					std::string error_line;
					while (std::getline(error_stream, error_line)) {
						std::cerr << "[FFmpeg stderr] " << error_line << std::endl;
					}
				});
				std::string line;
				while (std::getline(output_stream, line)) {
					const std::size_t pts_position = line.find("pts_time:");
					if (pts_position == std::string::npos) {
						continue;
					}
					try {
						const std::size_t value_start = pts_position + sizeof("pts_time:") - 1;
						const std::size_t value_end = line.find_first_of(" \t", value_start);
						const double current_sec = std::stod(line.substr(value_start, value_end - value_start));
						const int current_percent = std::min(
							static_cast<int>((current_sec / total_duration_sec) * 100.0),
							99
						);
						updateProgress(current_percent);
					}
					catch (const std::exception& e) {
						std::cerr << "Invalid FFmpeg progress line: " << line
							<< " (" << e.what() << ")" << std::endl;
					}
				}
				ffmpeg_process.wait();
				error_reader.join();
				const int exit_code = ffmpeg_process.exit_code();
				if (exit_code != 0) {
					std::cerr << "ffmpeg exited with code " << exit_code << " for video ID: " << video_id << std::endl;
					postEncodeResult(video_id, "failed", "ffmpeg exited with code " + std::to_string(exit_code));
					return 1;
				}
				std::cout << "Encoding and thumbnail generation completed successfully for video ID: " << video_id << std::endl;
				updateProgress(100);
			}
			catch (const std::exception& e) {
				std::cerr << "Encoding Error: " << e.what() << std::endl;
				postEncodeResult(video_id, "failed", "Encoding error: " + std::string(e.what()));
				return 1;
			}
			// WebTTファイルの生成
			std::ofstream vtt_file(base_dir + "thumbnails.vtt");
			vtt_file << "WEBVTT\n\n";
			int total_thumbnails = static_cast<int>(total_duration_sec / static_cast<double>(interval)) + 1;
			constexpr int images_per_sheet = 10 * 10;
			const int w = 160, h = 90;
			std::string api_path = "/api/videos/" + video_id + "/thumbnails/";
			for (int i = 0; i < total_thumbnails; ++i) {
				int sheet_index = (i / images_per_sheet) + 1;
				std::string image_name = std::format("thumbnail{:03d}.jpg", sheet_index);
				std::string start_time = formatTime(i * interval);
				std::string end_time = formatTime((i + 1) * interval);

				int index_in_sheet = i % images_per_sheet;
				int col = index_in_sheet % 10;
				int row = index_in_sheet / 10;

				int x = col * 160;
				int y = row * 90;

				vtt_file << start_time << " --> " << end_time << "\n";
				vtt_file << api_path << image_name << "#xywh=" << x << "," << y << "," << w << "," << h << "\n\n";
			}
			vtt_file.close();
			upload2MinIO(base_dir + "output.m4s", "videos", "hls/" + video_id + "/output.m4s");
			upload2MinIO(base_dir + "output.m3u8", "videos", "hls/" + video_id + "/output.m3u8");
			for (int i = 1; i <= (total_thumbnails / images_per_sheet) + 1; ++i) {
				std::string local_image_path = base_dir + std::format("thumbnail{:03d}.jpg", i);
				if (std::filesystem::exists(local_image_path)) {
					upload2MinIO(local_image_path, "videos", "hls/" + video_id + std::format("/thumbnail{:03d}.jpg", i));
				}
			}
			upload2MinIO(base_dir + "thumbnails.vtt", "videos", "hls/" + video_id + "/thumbnails.vtt");
			upload2MinIO(base_dir + "thumbnail.jpg", "videos", "hls/" + video_id + "/thumbnail.jpg");
			std::filesystem::remove_all("/tmp/playbacq_encode/" + video_id);

			deleteFromMinIO("videofiles", video_id + ".mp4");

			std::cout << "[JOB COMPLETED] Video ID: " << video_id << std::endl;

			postEncodeResult(video_id, "completed", "Encoding completed successfully", static_cast<int>(total_duration_sec));
		}
		catch (const sw::redis::Error& e) {
			std::cerr << "Redis Error: " << e.what() << std::endl;
			return 1;
		}
	}
	curl_global_cleanup();
	Aws::ShutdownAPI(options);
	return 0;
}

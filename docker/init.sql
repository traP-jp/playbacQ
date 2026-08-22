SET
	character_set_client = 'utf8mb4';
SET
	collation_connection = 'utf8mb4_general_ci';
CREATE DATABASE IF NOT EXISTS playbacq;
USE playbacq;

-- Grant privileges to the user (created by MYSQL_USER in docker-compose.yml)
GRANT ALL PRIVILEGES ON playbacq.* TO 'playbacq_user'@'%';
FLUSH PRIVILEGES;

CREATE TABLE IF NOT EXISTS videos (
	video_id VARCHAR(255) PRIMARY KEY,
	user_id VARCHAR(32),
	title TEXT NOT NULL,
	description TEXT,
	thumbnail_url TEXT,
	video_url TEXT NOT NULL,
	created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
	view_count INT NOT NULL DEFAULT 0,
	duration INT NOT NULL DEFAULT 0,
	like_count INT NOT NULL DEFAULT 0,
	status TINYINT UNSIGNED NOT NULL DEFAULT 0,
	is_external TINYINT UNSIGNED NOT NULL DEFAULT 0,
	type VARCHAR(32) NOT NULL DEFAULT 'internal'
);
CREATE TABLE IF NOT EXISTS external_video_statistics (
	video_id VARCHAR(255) PRIMARY KEY,
	provider VARCHAR(32) NOT NULL,
	view_count BIGINT UNSIGNED,
	like_count BIGINT UNSIGNED,
	comment_count BIGINT UNSIGNED,
	synced_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
	FOREIGN KEY (video_id) REFERENCES videos(video_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS external_video_metadata (
	video_id VARCHAR(255) PRIMARY KEY,
	provider VARCHAR(32) NOT NULL,
	metadata JSON NOT NULL,
	synced_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
	FOREIGN KEY (video_id) REFERENCES videos(video_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS comments (
	comment_id INT AUTO_INCREMENT PRIMARY KEY,
	video_id VARCHAR(255),
	user_id VARCHAR(32),
	comment TEXT NOT NULL,
	timestamp DOUBLE NOT NULL,
	created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
	command TEXT,
	status TINYINT UNSIGNED NOT NULL DEFAULT 0,
	FOREIGN KEY (video_id) REFERENCES videos(video_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS tags (
	tag_id INT AUTO_INCREMENT PRIMARY KEY,
	name VARCHAR(32) NOT NULL UNIQUE
);
CREATE TABLE IF NOT EXISTS video_tags (
	video_id VARCHAR(255),
	tag_id INT,
	status TINYINT UNSIGNED NOT NULL DEFAULT 0,
	PRIMARY KEY (video_id, tag_id),
	FOREIGN KEY (video_id) REFERENCES videos(video_id) ON DELETE CASCADE,
	FOREIGN KEY (tag_id) REFERENCES tags(tag_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS video_likes (
	video_id VARCHAR(255),
	user_id VARCHAR(32),
	created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
	PRIMARY KEY (video_id, user_id),
	FOREIGN KEY (video_id) REFERENCES videos(video_id) ON DELETE CASCADE
);
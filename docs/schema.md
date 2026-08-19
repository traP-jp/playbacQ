# DB schema

```mermaid
erDiagram  
  videos {
	varchar video_id PK
	varchar user_id FK
	text title
	text description
	text thumbnail_url
	text video_url
	datetime created_at
	int view_count
	int duration
	int like_count
	int comment_count
	TINYINT status
	TINYINT is_external
	varchar type
  }
  comments {
	int comment_id PK
	varchar video_id FK
	varchar user_id FK
	text comment
	double timestamp
	datetime created_at
	text option
	TINYINT status
  }

  video_likes {
	varchar video_id PK,FK
	varchar user_id PK,FK
	datetime created_at
  }
  
  tags {
	int tag_id PK
	varchar name
  }

  video_tags {
	varchar video_id PK,FK
	int tag_id PK,FK
	int status
  }
  
  videos||--o{comments : has
  videos||--o{video_tags : has
  tags||--o{video_tags : tagged_with
  videos||--o{video_likes : has
```


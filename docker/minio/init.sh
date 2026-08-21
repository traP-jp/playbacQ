#!/bin/sh

set -eu

echo "Waiting for MinIO to start..."
until mc alias set myminio http://minio:9000 "${MINIO_ROOT_USER}" "${MINIO_ROOT_PASSWORD}"; do
  echo "MinIO is not available yet. Retrying..."
  sleep 3
done

echo "Successfully connected to MinIO. Creating buckets and configuring events..."

# bucketを作成
mc mb myminio/videos --ignore-existing
mc mb myminio/videofiles --ignore-existing

mc anonymous set download myminio/videofiles

# バケットのイベント通知を設定 (ARNのIDはMINIO_NOTIFY_WEBHOOK_ENABLE_1の"1"と一致させる)
mc event add myminio/videofiles arn:minio:sqs::1:webhook --event put --ignore-existing

echo "Bucket created and webhook notification set up successfully."

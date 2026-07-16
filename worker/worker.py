import modal
import subprocess
import os

# playbacQ ディレクトリをコンテキストとして送る
base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
dockerfile_path = os.path.join(base_dir, "Dockerfile.worker")

image = modal.Image.from_dockerfile(
    dockerfile_path,
    context_mount=modal.Mount.from_local_dir(base_dir, remote_path="/context")
)

app = modal.App("playbacq-worker")

@app.function(
    image=image,
    # GPUはT4が動画エンコードに最適らしい
    gpu="T4",
    secrets=[modal.Secret.from_dotenv(__file__+"/../../.env")],
    # エンコードに1h以上かかるような動画はお断り
    timeout=3600
    )
def encode(video_id: str):
    # C++を実行
    subprocess.run(["/usr/local/bin/playbacq_worker", video_id])
@app.function()
@modal.web_endpoint(method="POST")
def encode_webhook(data: dict):
    video_id = data.get("video_id")
    if not video_id:
        return {"status": "error", "message": "video_id is required"}
    # 非同期で実行
    encode.spawn(video_id)
    return {"status": "started", "message": f"Encoding started for {video_id}"}
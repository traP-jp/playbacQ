# Playbacq
動画共有サービス

・フロントエンド：https://github.com/UnABC/playbacQ-UI

## 技術スタック
### Frontend
* **Framework**: Angular 19+
* **UI Library**: Angular Material
* **Video Player**: Plyr / HLS.js
* **Reactive Programming**: RxJS

### Backend
* **Language**: C++23
* **Framework**: Drogon (C++ Web Framework)
* **Storage**: MinIO (S3 Compatible Object Storage)
* **Database**: MySQL / Redis (View Count Caching & Job Queue)
* **Others**: AWS SDK for C++ (S3 Plugin)

## 環境構築
`.env`は`.env.example`を参考に設定

## 実行

### `make local-dev`

次のコマンドで、ローカル環境のビルドから起動までまとめて実行可能です。

```bash
make local-dev
```

内部では次の処理を行います。

1. MySQL、Redis、MinIO、バックエンドコンテナを起動
2. `BUILD_LOCAL_DEV=ON`と`USE_LOCAL_WORKER=ON`でCMake configure
3. `build`が未作成なら通常ビルド、作成済みならインクリメンタルビルド
4. `./build/playbacq`を起動
5. バックエンドの8080番ポートが利用可能になるまで待機


### 手動

開発用コンテナを起動して手動でビルド・実行することもできます。

```bash
docker compose up -d
docker compose exec backend bash
```

コンテナ内で実行

```bash
cmake -S . -B build -DBUILD_LOCAL_DEV=ON -DUSE_LOCAL_WORKER=ON
cmake --build build -j$(nproc)
./build/playbacq
```

#### VSCode CMake Tools

VSCodeのCmake Toolsでもビルド・実行可能です。

CMake Toolsには次を設定します。

```json
{
  "cmake.buildDirectory": "${workspaceFolder}/build",
  "cmake.configureSettings": {
    "CMAKE_BUILD_TYPE": "Debug",
    "BUILD_LOCAL_DEV": "ON",
    "USE_LOCAL_WORKER": "ON"
  },
  "cmake.buildBeforeRun": true,
  "cmake.debugConfig": {
    "cwd": "${workspaceFolder}"
  }
}
```

ビルドターゲットは`all`、実行・デバッグ対象は`playbacq`を選択します。
CMake Toolsから実行する場合は`make local-dev`を実行せず`docker compose up -d`を実行。

### 補助コマンド

バックエンドのログを表示します。

```bash
make local-dev-logs
```

開発用コンテナのシェルを開きます。。

```bash
make local-dev-shell
```

ローカル環境を停止します。

```bash
make local-dev-down
```

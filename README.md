# FMOD-PLUGINS

FMOD 用の Windows コーデックプラグイン集です。  
各コーデックのソースは `codecs/<name>/` に分離し、Visual Studio / MSBuild のプロジェクトはルートの単一ファイル `FMOD-PLUGINS.vcxproj` に統合しています。

## Layout

```text
FMOD-PLUGINS/
├─ FMOD-PLUGINS.sln
├─ FMOD-PLUGINS.vcxproj
├─ codecs/
│  ├─ ape/
│  ├─ mp4/
│  ├─ opus/
│  ├─ srla/
│  ├─ tak/
│  ├─ tta/
│  ├─ wma/
│  └─ wv/
├─ scripts/
│  └─ fetch-dependencies.ps1
└─ deps/   # generated, ignored
```

## Build

依存取得:

```powershell
.\scripts\fetch-dependencies.ps1
```

ビルド:

```powershell
msbuild FMOD-PLUGINS.vcxproj /m /p:Configuration=mp4 /p:Platform=x64
```

`Configuration` には `ape`, `mp4`, `opus`, `srla`, `tak`, `tta`, `wma`, `wv` を指定します。  
出力先は `build/<codec>/x64/codec_<codec>.dll` です。

## GitHub Actions

`.github/workflows/build.yml` は checkout 後に `scripts/fetch-dependencies.ps1` を実行し、そのまま各コーデックをビルドします。  
現在のデフォルト matrix は `ape`, `mp4`, `opus`, `srla`, `tta`, `wma`, `wv` です。

`tak` は上流が安定した直接アーカイブ URL を公開していないため、`TAK_SDK_URL` を与えた場合のみ依存取得を自動化する想定にしています。

## Codecs

- `mp4`: AAC / ALAC in MP4 container
- `ape`: Monkey's Audio
- `opus`: Ogg Opus
- `srla`: Soleil Rising Lossless Audio
- `tak`: Tom's lossless Audio Kompressor
- `tta`: The True Audio
- `wma`: Windows Media Audio
- `wv`: WavPack

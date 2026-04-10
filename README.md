# FMOD-PLUGINS

[![build](https://github.com/AoiKagase/FMOD-PLUGINS/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/AoiKagase/FMOD-PLUGINS/actions/workflows/build.yml)

FMOD 用の Windows コーデックプラグイン集です。

各コーデックの実装は `codecs/<name>/` に分離し、ビルド定義はルートの単一ソリューション / プロジェクトに集約しています。

## Overview

- 1 つのソリューション: `FMOD-PLUGINS.sln`
- 1 つのプロジェクト: `FMOD-PLUGINS.vcxproj`
- コーデックごとにソースを分離: `codecs/<name>/`
- 外部依存はリポジトリに同梱せず、`scripts/fetch-dependencies.ps1` で取得
- GitHub Actions から依存取得とビルドを自動実行

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

## Supported Codecs

| Configuration | Format |
| --- | --- |
| `ape` | Monkey's Audio |
| `mp4` | AAC / ALAC in MP4 container |
| `opus` | Ogg Opus |
| `srla` | Soleil Rising Lossless Audio |
| `tak` | Tom's lossless Audio Kompressor |
| `tta` | The True Audio |
| `wma` | Windows Media Audio |
| `wv` | WavPack |

## Build

### 1. Fetch dependencies

外部依存を取得して `deps/` 配下に展開します。

```powershell
.\scripts\fetch-dependencies.ps1
```

### 2. Build a codec

`Configuration` に対象コーデック名を指定してビルドします。

```powershell
msbuild FMOD-PLUGINS.vcxproj /m /p:Configuration=mp4 /p:Platform=x64
```

指定可能な `Configuration`:

```text
ape, mp4, opus, srla, tak, tta, wma, wv
```

出力先:

```text
build/<codec>/x64/codec_<codec>.dll
```

例:

```powershell
msbuild FMOD-PLUGINS.vcxproj /m /p:Configuration=opus /p:Platform=x64
```

## GitHub Actions

GitHub Actions の [build.yml](H:\sourcecode\FMOD-AAC-CODEC\.github\workflows\build.yml) は、checkout 後に依存取得を行い、そのまま各コーデックをビルドして DLL を artifact として保存します。

現在のデフォルト matrix:

```text
ape, mp4, opus, srla, tta, wma, wv
```

`tak` は上流が安定した直接アーカイブ URL を公開していないため、`TAK_SDK_URL` を与えた場合のみ依存取得を自動化する想定にしています。

## Notes

- `deps/`, `build/`, `obj/` は生成物であり、リポジトリには含めません。
- `tak` をビルドする場合は、事前に `TAK_SDK_URL` を設定して依存取得を実行してください。

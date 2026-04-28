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
| `mp4` | AAC (raw ADTS) / AAC in MP4 / ALAC in MP4 |
| `opus` | Ogg Opus |
| `srla` | [Soleil Rising Lossless Audio](https://github.com/aikiriao/SRLA)|
| `tak` | Tom's lossless Audio Kompressor |
| `tta` | The True Audio |
| `wma` | Windows Media Audio |
| `wv` | WavPack |

## Build

### 1. Fetch dependencies

外部依存を取得して `deps/` 配下に展開します。
FMOD SDK ヘッダは公開対象に含めないため、事前にローカルの FMOD Studio API インストール先を `FMOD_SDK_DIR` に指定してください。

```powershell
$env:FMOD_SDK_DIR = "C:\Program Files (x86)\FMOD SoundSystem\FMOD Studio API Windows"
```

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
- `fmod*.h` は FMOD SDK からローカルに取得するファイルであり、公開リポジトリには含めません。
- `tak` をビルドする場合は、事前に `TAK_SDK_URL` を設定して依存取得を実行してください。

## License

このリポジトリの自作コードは `MIT` です。ただし、リポジトリ全体または生成される各 DLL が
一律に `MIT` になるわけではありません。

- `fmod*.h` は Firelight / FMOD SDK の著作物であり、このリポジトリの `MIT` では再許諾しません。
- 各コーデックはビルド時に別ライセンスの依存物を取り込みます。依存物の著作権表示やライセンス文書の同梱が必要な場合があります。
- 特に `mp4` は `FAAD2` を使用し、`tta` は `libtta++` のソースを直接コンパイルします。これらは `MIT` 単独では扱えません。
- 依存ライセンスの概要と配布時の注意点は [`THIRD_PARTY_NOTICES.md`](H:\sourcecode\FMOD-AAC-CODEC\THIRD_PARTY_NOTICES.md) を参照してください。

配布物を公開する前に、対象 codec の依存ライセンス条件を個別に確認してください。

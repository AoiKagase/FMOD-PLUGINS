# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## プロジェクト概要

FMODオーディオライブラリ用のWindowsコーデックプラグイン集（x64）。FMOD Core APIのコーデックプラグインインターフェースを実装し、以下のフォーマットを再生可能にする：

| コーデック | 対応フォーマット | 依存ライブラリ |
|-----------|-------------|------------|
| ape       | Monkey's Audio (.ape) | Monkey's Audio SDK |
| mp4       | M4A/AAC/ALAC (.m4a) | FAAD2, Apple ALAC, minimp4 |
| opus      | Ogg Opus (.opus) | opus, opusfile, ogg |
| srla      | SRLA (.srla) | SRLA |
| tak       | TAK (.tak) | TAK SDK（要別途入手） |
| tta       | TTA (.tta) | libtta++ |
| wma       | WMA (.wma) | Windows Media Foundation |
| wv        | WavPack (.wv) | WavPack |

**mp4コーデックの制限:** メタデータ解析はiTunes作成のM4A/AACファイルに最適化。

## ビルド方法

Visual Studio 2026でビルドする。

### 事前準備（初回・依存更新時）

```powershell
.\scripts\fetch-dependencies.ps1
```

依存ライブラリをダウンロードし `deps/` に配置する。TAK SDKのみ事前に環境変数を設定：

```powershell
$env:TAK_SDK_URL = "https://..."  # TAK SDKのURL（未公開SDKのため別途入手）
```

### コマンドラインビルド（MSBuild）

```powershell
# 単一コーデック（Configuration にコーデック名を指定）
msbuild FMOD-PLUGINS.vcxproj /m /p:Configuration=mp4 /p:Platform=x64

# 出力先: build\<codec>\x64\codec_<codec>.dll
# 例:     build\mp4\x64\codec_mp4.dll
```

## アーキテクチャ

### プロジェクト構造

単一の `FMOD-PLUGINS.vcxproj` が全コーデックをビルド構成（`Configuration`）で切り替える。
各コーデックは独立した実装ファイルとプロパティファイルで構成される：

```
codecs/<name>/main.cpp       # FMODコーデックコールバック実装
codecs/<name>/codec.props    # インクルード/ライブラリパス、追加ソースファイル定義
```

FMOD Core APIのヘッダー（`fmod*.h`）はプロジェクトルートに配置済み。
依存ライブラリは `deps/` に `scripts/fetch-dependencies.ps1` で生成する（gitignore対象）。

### FMODコーデックコールバックパターン（全コーデック共通）

各コーデックが `FMOD_CODEC_DESCRIPTION` を定義し、以下のコールバックを実装する：

1. **Open** (`myCodec_open`): ファイル解析→デコーダ初期化→全フレームをPCMに事前デコード→バッファ蓄積
2. **Read** (`myCodec_read`): 事前デコード済みPCMバッファからデータを返す
3. **Seek/Position** (`myCodec_setposition`/`myCodec_getposition`): バッファ内位置管理
4. **Close** (`myCodec_close`): デコーダとバッファの解放

### mp4コーデックの詳細（`codecs/mp4/main.cpp`）

AACとALACの両形式を1つの実装で対応する。Open時にコーデック種別を判定：

- **AAC**: minimp4でコンテナ解析 → FAAD2で全フレームデコード
- **ALAC**: minimp4でコンテナ解析 → Apple ALACDecoderで全フレームデコード
  - `payload_bytes`バグ回避のため stco/stsz/stsc をファイルから直接読み取る

**主要な型:**
- `info`: コーデック状態（デコーダハンドル、PCMバッファ、フォーマット情報、メタデータ）
- `AlacFrameTable`: ALACフレームオフセット・サイズテーブル（stco/stsz/stscから構築）
- `PCMFormatInfo`: FMODとデコーダのPCMフォーマットのマッピング

**出力フォーマット対応（AAC）:**

| FMODフォーマット | FAAD2フォーマット | バイト/サンプル |
|-----------------|-----------------|--------------|
| PCMFLOAT | FAAD_FMT_FLOAT | 4 |
| PCM32 | FAAD_FMT_32BIT | 4 |
| PCM24 | FAAD_FMT_24BIT | 3 |
| PCM16 (デフォルト) | FAAD_FMT_16BIT | 2 |

## FMODプラグインの使用方法（C#）

```csharp
uint handle;
system.loadPlugin("codec_mp4.dll", out handle);
```

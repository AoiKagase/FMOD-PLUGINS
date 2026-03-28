# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## プロジェクト概要

FMODオーディオライブラリ用のカスタムコーデックプラグイン。iTunes作成のM4A/AACファイルを再生可能にするWindows DLL。FMOD Core APIのコーデックプラグインインターフェースを実装し、FAAD2デコーダーとminimp4パーサーを使用してAAC→PCM変換を行う。

**重要な制限:** タグ解析はiTunes作成のM4A/AACファイルに最適化されており、非iTunesファイルでは失敗する場合がある。

## ビルド方法

Visual Studio 2022でビルドする：

```
codec_mp4/codec_mp4.sln
```

ビルド構成：`Debug|Win32`、`Release|Win32`、`Debug|x64`、`Release|x64`

コマンドラインビルド（MSBuild）:
```bash
msbuild codec_mp4/codec_mp4.sln /p:Configuration=Release /p:Platform=x64
```

出力DLL：`codec_mp4/x64/Release/codec_mp4.dll`（または対応するディレクトリ）

## アーキテクチャ

### プラグイン構造

`main.cpp`が唯一の実装ファイル。FMODコーデックコールバックを以下の流れで実装：

1. **Open** (`myCodec_open`): MP4コンテナを解析→mdatボックスを特定→AACフレームデータ抽出→FAAD2初期化→全AACストリームをPCMにデコード→iTunesメタデータ抽出
2. **Read** (`myCodec_read`): デコード済みPCMバッファからデータを返す
3. **Seek** (`myCodec_setposition`/`myCodec_getposition`): 再生位置管理
4. **Close** (`myCodec_close`): FAAD2デコーダーとバッファの解放

### 主要な型・構造体

- `info`: コーデック状態（AAC デコーダハンドル、PCMバッファ、フォーマット情報、メタデータ）
- `MP4HEADER`: MP4ボックス解析用（size, header, data）
- `PCMFormatInfo`: FMODとFAAD2のPCMフォーマットのマッピング

### 出力フォーマット対応

| FMODフォーマット | FAAD2フォーマット | バイト/サンプル |
|-----------------|-----------------|--------------|
| PCMFLOAT | FAAD_FMT_FLOAT | 4 |
| PCM32 | FAAD_FMT_32BIT | 4 |
| PCM24 | FAAD_FMT_24BIT | 3 |
| PCM16 (デフォルト) | FAAD_FMT_16BIT | 2 |

### 依存ライブラリ

- **FAAD2**: `libfaad.lib`（プリコンパイル済み、`#pragma comment`でリンク）、`neaacdec.h`で宣言
- **minimp4**: ヘッダーオンリー（`minimp4.h`）、MP4コンテナ解析とメタデータ抽出
- **FMOD Core API**: ヘッダーのみ（`fmod*.h`）、コーデックプラグインインターフェース定義

## FMODプラグインの使用方法（C#）

```csharp
uint handle;
system.loadPlugin("codec_mp4.dll", out handle);
```

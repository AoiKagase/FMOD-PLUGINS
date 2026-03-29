# libtta++ (True Audio C++ Library)

## 入手方法

SourceForge から libtta++ 2.2 をダウンロードしてください:
https://sourceforge.net/projects/tta/files/tta++/

## 配置するファイル

ダウンロードしたアーカイブを展開し、以下のファイルをこの `libtta/` ディレクトリに配置:

```
libtta/libtta.h
libtta/libtta.cpp
libtta/filter.h
libtta/config.h
```

## ビルドについて

libtta++ はプロジェクトにソースを直接取り込む方式です。
別途 .lib を作成する必要はありません。
codec_tta.vcxproj が libtta.cpp を自動的にコンパイルします。

## バージョンについて

対応バージョン: **libtta++ 2.2**

TTA_io_callback 構造体が以下のシグネチャを持つバージョンが必要です:

```cpp
typedef struct _TTA_io_callback {
    TTAuint32 (*read)(struct _TTA_io_callback *io, TTAbyte *buffer, TTAuint32 size);
    TTAint64  (*seek)(struct _TTA_io_callback *io, TTAint64 offset);
} TTA_io_callback;
```

## 例外について

libtta++ は C++ 例外 (`tta::tta_exception`) を使用します。
main.cpp の `try/catch` ブロックでこれを処理しています。

## 対応ビット深度

| libtta bps | FMOD フォーマット |
|-----------|----------------|
| 8-16 bit  | PCM16          |
| 17-24 bit | PCM24          |
| 25-32 bit | PCM32          |

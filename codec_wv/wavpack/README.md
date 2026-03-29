# WavPack Library (libwavpack)

## 入手方法

公式サイトまたは GitHub から WavPack ソースを取得してください:

- 公式: https://www.wavpack.com/
- GitHub: https://github.com/dbry/WavPack

## ビルド方法 (CMake)

```bash
git clone https://github.com/dbry/WavPack.git
cd WavPack
cmake -S . -B build -DBUILD_SHARED_LIBS=OFF -DWAVPACK_BUILD_PROGRAMS=OFF
cmake --build build --config Release
cmake --build build --config Debug
```

## 配置するファイル

```
wavpack/include/wavpack.h                ← ヘッダ
wavpack/lib/x64/Release/wavpack.lib      ← x64 Release ビルド用
wavpack/lib/x64/Debug/wavpack.lib        ← x64 Debug ビルド用
wavpack/lib/Win32/Release/wavpack.lib    ← Win32 Release ビルド用
wavpack/lib/Win32/Debug/wavpack.lib      ← Win32 Debug ビルド用
```

CMake ビルド後の .lib ファイルは以下にあります:
- `build/Release/wavpack.lib`
- `build/Debug/wavpack.lib`

## バージョンについて

対応バージョン: **WavPack 5.x**

`WavpackStreamReader64` および `WavpackOpenFileInputEx64` を使用します。
WavPack 4.x 以前には `WavpackStreamReader64` が存在しないため使用できません。

## RuntimeLibrary について

vcxproj は `/MT` (MultiThreaded) でビルドします。
CMake でビルドする場合は以下のオプションを追加してください:

```bash
cmake -S . -B build -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>"
```

## フォーマット検出

WavPack ファイルは先頭 4 バイトが `wvpk` です。
マジックバイトが一致しない場合は `FMOD_ERR_FORMAT` を返します。

## 対応ビット深度・フォーマット

| WavPack 形式         | FMOD フォーマット |
|---------------------|----------------|
| 整数 1-16 bit        | PCM16          |
| 整数 17-24 bit       | PCM24          |
| 整数 25-32 bit       | PCM32          |
| 浮動小数点 (float)   | PCMFLOAT       |

## タグについて

WavPack は APEv2 タグを内蔵しており、SDK の `WavpackGetTagItem` で直接読み取れます。
対応タグ: `Title` / `Artist` / `Album`

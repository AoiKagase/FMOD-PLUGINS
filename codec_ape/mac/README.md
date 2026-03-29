# Monkey's Audio SDK (MAC Library)

## 入手方法

公式サイトからSDKをダウンロードしてください:
https://www.monkeysaudio.com/developers.html

## 配置するファイル

SDKをダウンロード後、以下のようにファイルを配置してください。

### ヘッダファイル (mac/ 直下)
- `All.h`
- `MACLib.h`
- `Version.h`
- `Warnings.h`
- `WindowsEnvironment.h`
- `CharacterHelper.h`
- `SmartPtr.h`

### ライブラリファイル

SDK をビルドするか、ビルド済みバイナリを以下に配置:

```
mac/lib/x64/Release/MACLib.lib <-
mac/lib/x64/Debug/MACLib.lib
mac/lib/Win32/Release/MACLib.lib
mac/lib/Win32/Debug/MACLib.lib
```

## バージョンについて

`main.cpp` は MAC SDK **10.x 系** (64bit int API) を対象としています。

**古いバージョン (8.x 以前)** を使用する場合は `main.cpp` 内の
`int64` を `int` に変更してください (`GetInfo`/`GetData` の引数・戻り値)。

## SDK のビルド方法 (ソースから)

SDK に含まれる Visual Studio ソリューションを使ってビルドする:

```
Projects/Windows/MACLib/MACLib.sln
```

ビルド後、生成された `.lib` を上記のパスに配置してください。

# TAK SDK (tak_deco_lib)

## 入手方法

公式サイトから TAK SDK をダウンロードしてください:
http://www.thbeck.de/Tak/Tak.html

## 配置するファイル

```
tak/tak_deco_lib.h
```

## ランタイム DLL

TAK SDK は DLL 方式のため、**実行時に以下が必要**です:

```
codec_tak.dll と同じディレクトリに tak_deco_lib.dll を配置
```

SDK に含まれる `tak_deco_lib.dll` (x64版) をコピーしてください。

## バージョンについて

対応バージョン: **TAK SDK 2.3.x**

main.cpp は以下の構造体を使用します:
```c
TtaStreamInfo.Audio.NumSamples  // TtaInt64 型を期待
TtaStreamInfo.Audio.SampleRate
TtaStreamInfo.Audio.BitsPerSample
TtaStreamInfo.Audio.ChannelNum
```

古い SDK で `NumSamples` が `TtaInt32` の場合は
main.cpp の `int64_t nFrames = si.Audio.NumSamples;` を
`int32_t nFrames = si.Audio.NumSamples;` に変更してください。

## フォーマット検出について

TAK は固定マジックバイトを持たないため、SDK の
`tak_SSD_Create_FromReader` がフォーマット検証を行います。
非 TAK ファイルに対しては `res.Error != tak_Err_Ok` で弾きます。

## 対応ビット深度

| TAK BitsPerSample | FMOD フォーマット |
|------------------|----------------|
| 8-16 bit         | PCM16          |
| 17-24 bit        | PCM24 (20bit含む)|
| 25-32 bit        | PCM32          |

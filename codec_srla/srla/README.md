# SRLA ライブラリ

**SRLA** (Soleil Rising Lossless Audio codec) のプリコンパイル済みライブラリ一式です。

- **リポジトリ**: https://github.com/aikiriao/SRLA
- **ライセンス**: MIT License

## ディレクトリ構成

```
srla/
├── include/
│   ├── srla.h           - 共通定義 (SRLAHeader, SRLAApiResult 等)
│   ├── srla_decoder.h   - デコーダ API
│   ├── srla_encoder.h   - エンコーダ API ※要らないかも
│   └── srla_stdint.h    - 整数型定義
└── libs/
    ├── srladec.lib      - デコーダライブラリ
    └── srlacodec.lib    - コーデック共通ライブラリ
```

## ファイルフォーマット

| 項目 | 内容 |
|------|------|
| 拡張子 | `.srl` |
| ヘッダサイズ | 29 バイト (`SRLA_HEADER_SIZE`) |
| 最大チャンネル数 | 8 (`SRLA_MAX_NUM_CHANNELS`) |
| 最大係数次数 | 255 (`SRLA_MAX_COEFFICIENT_ORDER`) |

## デコーダ API 概要

```c
// ヘッダ解析
SRLAApiResult SRLADecoder_DecodeHeader(
    const uint8_t *data, uint32_t data_size, struct SRLAHeader *header);

// ワークサイズ計算
int32_t SRLADecoder_CalculateWorkSize(const struct SRLADecoderConfig *config);

// デコーダ作成 / 破棄
struct SRLADecoder* SRLADecoder_Create(
    const struct SRLADecoderConfig *config, void *work, int32_t work_size);
void SRLADecoder_Destroy(struct SRLADecoder *decoder);

// ヘッダセット
SRLAApiResult SRLADecoder_SetHeader(
    struct SRLADecoder *decoder, const struct SRLAHeader *header);

// 全ブロック一括デコード
SRLAApiResult SRLADecoder_DecodeWhole(
    struct SRLADecoder *decoder,
    const uint8_t *data, uint32_t data_size,
    int32_t **buffer, uint32_t buffer_num_channels, uint32_t buffer_num_samples);

// 単一ブロックデコード
SRLAApiResult SRLADecoder_DecodeBlock(
    struct SRLADecoder *decoder,
    const uint8_t *data, uint32_t data_size,
    int32_t **buffer, uint32_t buffer_num_channels, uint32_t buffer_num_samples,
    uint32_t *decode_size, uint32_t *num_decode_samples);
```

デコード結果は `int32_t**`（チャンネル別配列）で返されます。サンプル値の範囲は `[-2^(bps-1), 2^(bps-1)-1]` です。

## ライブラリのビルド方法

ソースから `.lib` を再ビルドする場合:

```bash
git clone https://github.com/aikiriao/SRLA.git
cd SRLA
cmake -B build
cmake --build build --config Release
```

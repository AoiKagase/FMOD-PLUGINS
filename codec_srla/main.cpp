// main.cpp
// FMOD SRLA コーデックプラグイン
//
// =========================================================
// 外部ライブラリ: SRLA (Soleil Rising Lossless Audio Codec)
// 入手先: https://github.com/aikiriao/SRLA
// ライセンス: MIT License
//
// 必要なファイル:
//   srla/include/srla.h
//   srla/include/srla_decoder.h
//   srla/include/srla_stdint.h
//   srla/libs/srladec.lib
//   srla/libs/srlacodec.lib
// =========================================================

#include <memory>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>

#include "srla/include/srla_decoder.h"

#include "../fmod.h"
#include "../fmod_codec.h"

// =========================================================
// PCMフォーマット情報
// =========================================================
struct SRLAPCMFormat
{
    FMOD_SOUND_FORMAT fmodFormat;
    int               bytesPerSample;
};

static SRLAPCMFormat resolveSRLAFormat(uint16_t bps)
{
    if (bps <= 16) return { FMOD_SOUND_FORMAT_PCM16, 2 };
    if (bps <= 24) return { FMOD_SOUND_FORMAT_PCM24, 3 };
    return               { FMOD_SOUND_FORMAT_PCM32, 4 };
}

// =========================================================
// コーデック状態
// =========================================================
struct SRLAInfo
{
    std::vector<uint8_t>  buffer;
    uint64_t              position       = 0;
    int                   channels       = 0;
    int                   sampleRate     = 0;
    int                   bytesPerSample = 0;
    FMOD_SOUND_FORMAT     fmodFormat     = FMOD_SOUND_FORMAT_PCM16;
    FMOD_CODEC_WAVEFORMAT waveFormat     = {};
};

// =========================================================
// int32_t** チャンネル別バッファ → インターリーブ PCM 変換
// =========================================================
static void convertToInterleaved(
    int32_t** ch, uint32_t numChannels, uint32_t numSamples,
    uint8_t* dst, const SRLAPCMFormat& fmt)
{
    for (uint32_t s = 0; s < numSamples; ++s)
    {
        for (uint32_t c = 0; c < numChannels; ++c)
        {
            int32_t v = ch[c][s];
            if (fmt.bytesPerSample == 2)
            {
                int16_t v16 = static_cast<int16_t>(v);
                std::memcpy(dst, &v16, 2);
            }
            else if (fmt.bytesPerSample == 3)
            {
                dst[0] = static_cast<uint8_t>( v        & 0xFF);
                dst[1] = static_cast<uint8_t>((v >>  8) & 0xFF);
                dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            }
            else
            {
                std::memcpy(dst, &v, 4);
            }
            dst += fmt.bytesPerSample;
        }
    }
}

// =========================================================
// open コールバック
// =========================================================
static FMOD_RESULT F_CALL srlaCodec_open(FMOD_CODEC_STATE* codec,
    FMOD_MODE /*usermode*/, FMOD_CREATESOUNDEXINFO* /*userexinfo*/)
{
    if (!codec) return FMOD_ERR_INTERNAL;

    unsigned int fileSize = 0;
    codec->functions->size(codec, &fileSize);
    if (fileSize < SRLA_HEADER_SIZE) return FMOD_ERR_FILE_EOF;

    // ファイル全体を読み込む
    std::vector<uint8_t> fileData(fileSize);
    unsigned int rb = 0;
    codec->functions->seek(codec, 0, FMOD_CODEC_SEEK_METHOD_SET);
    codec->functions->read(codec, fileData.data(), fileSize, &rb);
    if (rb < SRLA_HEADER_SIZE) return FMOD_ERR_FORMAT;

    // ヘッダデコード（フォーマット検出）
    SRLAHeader header = {};
    if (SRLADecoder_DecodeHeader(fileData.data(), rb, &header) != SRLA_APIRESULT_OK)
        return FMOD_ERR_FORMAT;

    if (header.num_channels == 0 || header.num_samples == 0 || header.sampling_rate == 0)
        return FMOD_ERR_FORMAT;

    // デコーダ設定
    SRLADecoderConfig config      = {};
    config.max_num_channels       = header.num_channels;
    config.max_num_parameters     = SRLA_MAX_COEFFICIENT_ORDER;
    config.check_checksum         = 1;

    const int32_t workSize = SRLADecoder_CalculateWorkSize(&config);
    if (workSize < 0) return FMOD_ERR_INTERNAL;

    std::vector<uint8_t> work(static_cast<size_t>(workSize));
    SRLADecoder* decoder = SRLADecoder_Create(&config, work.data(), workSize);
    if (!decoder) return FMOD_ERR_INTERNAL;

    if (SRLADecoder_SetHeader(decoder, &header) != SRLA_APIRESULT_OK)
    {
        SRLADecoder_Destroy(decoder);
        return FMOD_ERR_FORMAT;
    }

    // チャンネル別デコードバッファ確保
    const uint32_t numCh      = header.num_channels;
    const uint32_t numSamples = header.num_samples;

    std::vector<std::vector<int32_t>> chBufs(numCh,
        std::vector<int32_t>(numSamples, 0));
    std::vector<int32_t*> chPtrs(numCh);
    for (uint32_t c = 0; c < numCh; ++c)
        chPtrs[c] = chBufs[c].data();

    const SRLAApiResult res = SRLADecoder_DecodeWhole(
        decoder,
        fileData.data(), rb,
        chPtrs.data(), numCh, numSamples);

    SRLADecoder_Destroy(decoder);

    if (res != SRLA_APIRESULT_OK) return FMOD_ERR_FILE_BAD;

    // PCMフォーマット決定・インターリーブ変換
    const SRLAPCMFormat fmt = resolveSRLAFormat(header.bits_per_sample);
    const size_t totalBytes =
        static_cast<size_t>(numSamples) * numCh * fmt.bytesPerSample;

    std::vector<uint8_t> pcmBuffer(totalBytes);
    convertToInterleaved(chPtrs.data(), numCh, numSamples,
                         pcmBuffer.data(), fmt);

    // コーデック状態確定
    auto* x           = new SRLAInfo();
    x->buffer         = std::move(pcmBuffer);
    x->position       = 0;
    x->channels       = static_cast<int>(numCh);
    x->sampleRate     = static_cast<int>(header.sampling_rate);
    x->bytesPerSample = fmt.bytesPerSample;
    x->fmodFormat     = fmt.fmodFormat;

    x->waveFormat.format       = fmt.fmodFormat;
    x->waveFormat.channels     = static_cast<int>(numCh);
    x->waveFormat.frequency    = static_cast<int>(header.sampling_rate);
    x->waveFormat.pcmblocksize = static_cast<int>(numCh) * fmt.bytesPerSample;
    x->waveFormat.lengthpcm    = numSamples;

    codec->plugindata = x;
    codec->waveformat = &x->waveFormat;

    return FMOD_OK;
}

// =========================================================
// close コールバック
// =========================================================
static FMOD_RESULT F_CALL srlaCodec_close(FMOD_CODEC_STATE* codec)
{
    delete reinterpret_cast<SRLAInfo*>(codec->plugindata);
    codec->plugindata = nullptr;
    return FMOD_OK;
}

// =========================================================
// read コールバック
// =========================================================
static FMOD_RESULT F_CALL srlaCodec_read(FMOD_CODEC_STATE* codec,
    void* buffer, unsigned int sizebytes, unsigned int* bytesread)
{
    auto* x = reinterpret_cast<SRLAInfo*>(codec->plugindata);
    const uint64_t bufSize = static_cast<uint64_t>(x->buffer.size());
    if (x->position >= bufSize)
    {
        *bytesread = 0;
        return FMOD_ERR_FILE_EOF;
    }
    const uint64_t    remaining = bufSize - x->position;
    const unsigned int toCopy   = static_cast<unsigned int>(
        std::min(static_cast<uint64_t>(sizebytes) * x->channels * x->bytesPerSample,
                 remaining));
    std::memcpy(buffer, x->buffer.data() + x->position, toCopy);
    x->position += toCopy;
    *bytesread   = toCopy;
    return FMOD_OK;
}

// =========================================================
// その他コールバック
// =========================================================
static FMOD_RESULT F_CALL srlaCodec_getlength(FMOD_CODEC_STATE* codec,
    unsigned int* length, FMOD_TIMEUNIT lengthtype)
{
    auto* x = reinterpret_cast<SRLAInfo*>(codec->plugindata);
    if (lengthtype == FMOD_TIMEUNIT_PCMBYTES)
        *length = static_cast<unsigned int>(x->buffer.size());
    return FMOD_OK;
}

static FMOD_RESULT F_CALL srlaCodec_setposition(FMOD_CODEC_STATE* codec,
    int /*subsound*/, unsigned int position, FMOD_TIMEUNIT postype)
{
    auto* x = reinterpret_cast<SRLAInfo*>(codec->plugindata);
    if (postype == FMOD_TIMEUNIT_PCMBYTES)
        x->position = std::min(static_cast<uint64_t>(position),
                               static_cast<uint64_t>(x->buffer.size()));
    return FMOD_OK;
}

static FMOD_RESULT F_CALL srlaCodec_getposition(FMOD_CODEC_STATE* codec,
    unsigned int* position, FMOD_TIMEUNIT postype)
{
    auto* x = reinterpret_cast<SRLAInfo*>(codec->plugindata);
    if (postype == FMOD_TIMEUNIT_PCMBYTES)
        *position = static_cast<unsigned int>(x->position);
    return FMOD_OK;
}

static FMOD_RESULT F_CALL srlaCodec_soundcreated(FMOD_CODEC_STATE* /*codec*/,
    int /*subsound*/, FMOD_SOUND* /*sound*/)
{
    return FMOD_OK;
}

static FMOD_RESULT F_CALL srlaCodec_getwaveformat(FMOD_CODEC_STATE* codec,
    int index, FMOD_CODEC_WAVEFORMAT* waveformat)
{
    if (index != 0) return FMOD_ERR_FORMAT;
    auto* x     = reinterpret_cast<SRLAInfo*>(codec->plugindata);
    *waveformat = x->waveFormat;
    return FMOD_OK;
}

// =========================================================
// プラグイン登録
// =========================================================
static FMOD_CODEC_DESCRIPTION s_srlaCodecDesc =
{
    FMOD_CODEC_PLUGIN_VERSION,
    "FMOD SRLA (Soleil Rising Lossless Audio) Codec",
    0x00010000,
    1,
    FMOD_TIMEUNIT_PCMBYTES,
    &srlaCodec_open,
    &srlaCodec_close,
    &srlaCodec_read,
    &srlaCodec_getlength,
    &srlaCodec_setposition,
    &srlaCodec_getposition,
    &srlaCodec_soundcreated,
    &srlaCodec_getwaveformat
};

#ifdef __cplusplus
extern "C" {
#endif
    __declspec(dllexport) FMOD_CODEC_DESCRIPTION* F_API FMODGetCodecDescription()
    {
        return &s_srlaCodecDesc;
    }
#ifdef __cplusplus
}
#endif

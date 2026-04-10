// main.cpp
// FMOD Opus コーデックプラグイン
//
// =========================================================
// 外部ライブラリ: opusfile (libopus + libogg)
//
// このプロジェクトでは deps/installed/x64-release に配置された
// opusfile / opus / ogg の静的ライブラリを利用する。
// =========================================================

#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <opusfile.h>

#include "fmod.h"

// =========================================================
// FMOD → opusfile IO ブリッジ
// =========================================================
struct FMODOpusIO
{
    FMOD_CODEC_STATE* codec;
    int64_t           pos;
    int64_t           length;
};

static int opus_read_cb(void* stream, unsigned char* ptr, int nbytes)
{
    auto*        io = static_cast<FMODOpusIO*>(stream);
    unsigned int rb = 0;
    io->codec->functions->read(io->codec, ptr, static_cast<unsigned int>(nbytes), &rb);
    io->pos += static_cast<int64_t>(rb);
    return static_cast<int>(rb);
}

static int opus_seek_cb(void* stream, opus_int64 offset, int whence)
{
    auto*   io = static_cast<FMODOpusIO*>(stream);
    int64_t newpos;
    switch (whence)
    {
    case SEEK_SET: newpos = static_cast<int64_t>(offset);               break;
    case SEEK_CUR: newpos = io->pos + static_cast<int64_t>(offset);     break;
    case SEEK_END: newpos = io->length + static_cast<int64_t>(offset);  break;
    default: return -1;
    }
    if (newpos < 0) return -1;
    io->codec->functions->seek(io->codec,
        static_cast<unsigned int>(newpos), FMOD_CODEC_SEEK_METHOD_SET);
    io->pos = newpos;
    return 0;
}

static opus_int64 opus_tell_cb(void* stream)
{
    return static_cast<opus_int64>(static_cast<FMODOpusIO*>(stream)->pos);
}

static const OpusFileCallbacks g_opusCb =
{
    opus_read_cb,
    opus_seek_cb,
    opus_tell_cb,
    nullptr  // close - FMODが管理するため不要
};

// =========================================================
// コーデック状態
// =========================================================
struct OpusInfo
{
    std::vector<uint8_t>  buffer;       // float PCM をバイト列として保持
    uint64_t              position  = 0; // バイト単位のオフセット
    int                   channels  = 0;
    FMOD_CODEC_WAVEFORMAT waveFormat = {};
};

// =========================================================
// タグ取得ヘルパー (Vorbis Comment 形式)
// =========================================================
static std::string opusGetTag(const OpusTags* tags, const char* key)
{
    if (!tags) return {};
    const char* v = opus_tags_query(tags, key, 0);
    if (v) return std::string(v);
    return {};
}

// =========================================================
// open コールバック
// =========================================================
static FMOD_RESULT F_CALL opusCodec_open(FMOD_CODEC_STATE* codec,
    FMOD_MODE /*usermode*/, FMOD_CREATESOUNDEXINFO* /*userexinfo*/)
{
    if (!codec) return FMOD_ERR_INTERNAL;

    // ファイルサイズ取得
    unsigned int totalSize = 0;
    codec->functions->size(codec, &totalSize);
    if (totalSize < 4) return FMOD_ERR_FILE_EOF;

    // OGG マジックバイト確認 ("OggS")
    uint8_t      magic[4] = {};
    unsigned int rb       = 0;
    codec->functions->read(codec, magic, 4, &rb);
    if (rb < 4 || memcmp(magic, "OggS", 4) != 0) return FMOD_ERR_FORMAT;
    codec->functions->seek(codec, 0, FMOD_CODEC_SEEK_METHOD_SET);

    // IO ブリッジ設定
    FMODOpusIO io = {};
    io.codec  = codec;
    io.pos    = 0;
    io.length = static_cast<int64_t>(totalSize);

    // opusfile でオープン (Opus 以外は失敗する)
    int          err = 0;
    OggOpusFile* of  = op_open_callbacks(&io, &g_opusCb, nullptr, 0, &err);
    if (!of) return FMOD_ERR_FORMAT;

    // ストリーム情報取得
    // Opus は常に 48000 Hz で出力する
    const int        channels     = op_channel_count(of, -1);
    const int        sampleRate   = 48000;
    const opus_int64 totalSamples = op_pcm_total(of, -1);

    if (totalSamples <= 0 || channels <= 0 || channels > 255)
    {
        op_free(of);
        return FMOD_ERR_FORMAT;
    }

    // 全フレームを float でデコード → uint8_t バッファに格納
    const size_t totalBytes = static_cast<size_t>(totalSamples) * channels * sizeof(float);
    std::vector<uint8_t> pcmBuffer(totalBytes);

    float*       dst         = reinterpret_cast<float*>(pcmBuffer.data());
    opus_int64   decoded     = 0;
    // Opus の最大フレームサイズは 120ms @ 48kHz = 5760 サンプル
    const int    CHUNK       = 5760;
    std::vector<float> tmp(static_cast<size_t>(CHUNK) * channels);

    while (totalSamples >= decoded)
    {
        const int got = op_read_float(of, tmp.data(),
            static_cast<int>(tmp.size()), nullptr);
        if (got <= 0) break;
        memcpy(dst, tmp.data(), static_cast<size_t>(got) * channels * sizeof(float));
        dst     += static_cast<size_t>(got) * channels;
        decoded += got;
    }

    // タグ読み取り (Vorbis Comment)
    const OpusTags* tags   = op_tags(of, -1);
    const auto      title  = opusGetTag(tags, "TITLE");
    const auto      artist = opusGetTag(tags, "ARTIST");
    const auto      album  = opusGetTag(tags, "ALBUM");

    op_free(of);

    // =========================================================
    // コーデック状態確定
    // =========================================================
    auto* x      = new OpusInfo();
    x->buffer    = std::move(pcmBuffer);
    x->position  = 0;
    x->channels  = channels;

    x->waveFormat.format       = FMOD_SOUND_FORMAT_PCMFLOAT;
    x->waveFormat.channels     = channels;
    x->waveFormat.frequency    = sampleRate;
    x->waveFormat.pcmblocksize = channels * static_cast<int>(sizeof(float));
    x->waveFormat.lengthpcm   = static_cast<unsigned int>(decoded);

    codec->plugindata = x;
    codec->waveformat = &x->waveFormat;

    // タグを FMOD に設定
    auto setTag = [&](const std::string& val, const char* key)
    {
        if (val.empty()) return;
        codec->functions->metadata(codec,
            FMOD_TAGTYPE_USER, const_cast<char*>(key),
            const_cast<char*>(val.c_str()),
            static_cast<unsigned int>(val.size() + 1),
            FMOD_TAGDATATYPE_STRING, 0);
    };
    setTag(title,  "Title");
    setTag(artist, "Artist");
    setTag(album,  "Album");

    return FMOD_OK;
}

// =========================================================
// close コールバック
// =========================================================
static FMOD_RESULT F_CALL opusCodec_close(FMOD_CODEC_STATE* codec)
{
    delete reinterpret_cast<OpusInfo*>(codec->plugindata);
    codec->plugindata = nullptr;
    return FMOD_OK;
}

// =========================================================
// read コールバック
// sizebytes は FMOD から渡される PCM フレーム数
// 戻り値 *bytesread は実際にコピーしたバイト数
// =========================================================
static FMOD_RESULT F_CALL opusCodec_read(FMOD_CODEC_STATE* codec,
    void* buffer, unsigned int sizebytes, unsigned int* bytesread)
{
    auto*          x           = reinterpret_cast<OpusInfo*>(codec->plugindata);
    const uint64_t bufSize     = static_cast<uint64_t>(x->buffer.size());
    if (x->position >= bufSize)
    {
        *bytesread = 0;
        return FMOD_ERR_FILE_EOF;
    }
    const uint64_t  remaining = bufSize - x->position;
    const uint64_t  toRequest = static_cast<uint64_t>(sizebytes)
                                * x->channels * sizeof(float);
    const unsigned int toCopy = static_cast<unsigned int>(
        std::min(toRequest, remaining));
    memcpy(buffer, x->buffer.data() + x->position, toCopy);
    x->position += toCopy;
    *bytesread   = toCopy;
    return FMOD_OK;
}

// =========================================================
// setposition コールバック
// =========================================================
static FMOD_RESULT F_CALL opusCodec_setposition(FMOD_CODEC_STATE* codec,
    int /*subsound*/, unsigned int position, FMOD_TIMEUNIT postype)
{
    auto* x = reinterpret_cast<OpusInfo*>(codec->plugindata);
    if (postype == FMOD_TIMEUNIT_PCMBYTES)
        x->position = std::min(static_cast<uint64_t>(position),
                               static_cast<uint64_t>(x->buffer.size()));
    return FMOD_OK;
}

// =========================================================
// getposition コールバック
// =========================================================
static FMOD_RESULT F_CALL opusCodec_getposition(FMOD_CODEC_STATE* codec,
    unsigned int* position, FMOD_TIMEUNIT postype)
{
    auto* x = reinterpret_cast<OpusInfo*>(codec->plugindata);
    if (postype == FMOD_TIMEUNIT_PCMBYTES)
        *position = static_cast<unsigned int>(x->position);
    return FMOD_OK;
}

// =========================================================
// getlength コールバック
// =========================================================
static FMOD_RESULT F_CALL opusCodec_getlength(FMOD_CODEC_STATE* codec,
    unsigned int* length, FMOD_TIMEUNIT lengthtype)
{
    auto* x = reinterpret_cast<OpusInfo*>(codec->plugindata);
    if (lengthtype == FMOD_TIMEUNIT_PCMBYTES)
        *length = static_cast<unsigned int>(x->buffer.size());
    return FMOD_OK;
}

// =========================================================
// soundcreated / getwaveformat コールバック
// =========================================================
static FMOD_RESULT F_CALL opusCodec_soundcreated(FMOD_CODEC_STATE* /*codec*/,
    int /*subsound*/, FMOD_SOUND* /*sound*/)
{
    return FMOD_OK;
}

static FMOD_RESULT F_CALL opusCodec_getwaveformat(FMOD_CODEC_STATE* codec,
    int index, FMOD_CODEC_WAVEFORMAT* waveformat)
{
    if (index != 0) return FMOD_ERR_FORMAT;
    auto* x    = reinterpret_cast<OpusInfo*>(codec->plugindata);
    *waveformat = x->waveFormat;
    return FMOD_OK;
}

// =========================================================
// プラグイン登録
// =========================================================
static FMOD_CODEC_DESCRIPTION s_opusCodecDesc =
{
    FMOD_CODEC_PLUGIN_VERSION,
    "FMOD Opus Codec",
    0x00010000,
    1,
    FMOD_TIMEUNIT_PCMBYTES,
    &opusCodec_open,
    &opusCodec_close,
    &opusCodec_read,
    &opusCodec_getlength,
    &opusCodec_setposition,
    &opusCodec_getposition,
    &opusCodec_soundcreated,
    &opusCodec_getwaveformat
};

#ifdef __cplusplus
extern "C" {
#endif
    __declspec(dllexport) FMOD_CODEC_DESCRIPTION* F_API FMODGetCodecDescription()
    {
        return &s_opusCodecDesc;
    }
#ifdef __cplusplus
}
#endif

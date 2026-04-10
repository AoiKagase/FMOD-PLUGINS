// main.cpp
// FMOD WavPack コーデックプラグイン
//
// =========================================================
// 外部ライブラリ: WavPack Library (libwavpack)
// 入手先: https://www.wavpack.com/
//         https://github.com/dbry/WavPack
//
// 必要なファイル:
//   deps/src/wv/include/wavpack.h
//   deps/lib/wv/x64/Release/libwavpack.lib
//
// ビルド方法 (CMake):
//   cmake -S . -B build -DBUILD_SHARED_LIBS=OFF
//   cmake --build build --config Release
// =========================================================

#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <wavpack.h>

#include "fmod.h"
#include "fmod_codec.h"

// =========================================================
// FMOD → WavPack IO ブリッジ
// =========================================================
struct FMODWvIO
{
    FMOD_CODEC_STATE* codec;
    uint64_t          pos;
    uint64_t          length;
    int               pushback; // push_back_byte 用、-1 = なし
};

static int32_t wv_read(void* id, void* data, int32_t bcount)
{
    FMODWvIO* io  = reinterpret_cast<FMODWvIO*>(id);
    uint8_t*  buf = reinterpret_cast<uint8_t*>(data);
    int32_t   count = 0;

    if (io->pushback >= 0 && bcount > 0)
    {
        buf[0]      = static_cast<uint8_t>(io->pushback);
        io->pushback = -1;
        buf++;
        bcount--;
        count = 1;
    }
    if (bcount > 0)
    {
        unsigned int rb = 0;
        io->codec->functions->read(io->codec, buf, static_cast<unsigned int>(bcount), &rb);
        io->pos += rb;
        count   += static_cast<int32_t>(rb);
    }
    return count;
}

static int32_t wv_write(void* /*id*/, void* /*data*/, int32_t /*bcount*/) { return 0; }

static int64_t wv_get_pos(void* id)
{
    return static_cast<int64_t>(reinterpret_cast<FMODWvIO*>(id)->pos);
}

static int wv_set_pos_abs(void* id, int64_t pos)
{
    FMODWvIO* io = reinterpret_cast<FMODWvIO*>(id);
    io->codec->functions->seek(io->codec, static_cast<unsigned int>(pos), FMOD_CODEC_SEEK_METHOD_SET);
    io->pos      = static_cast<uint64_t>(pos);
    io->pushback = -1;
    return 0;
}

static int wv_set_pos_rel(void* id, int64_t delta, int mode)
{
    FMODWvIO* io = reinterpret_cast<FMODWvIO*>(id);
    int64_t newpos;
    switch (mode)
    {
    case SEEK_SET: newpos = delta; break;
    case SEEK_CUR: newpos = static_cast<int64_t>(io->pos) + delta; break;
    case SEEK_END: newpos = static_cast<int64_t>(io->length) + delta; break;
    default: return -1;
    }
    if (newpos < 0) return -1;
    io->codec->functions->seek(io->codec, static_cast<unsigned int>(newpos), FMOD_CODEC_SEEK_METHOD_SET);
    io->pos      = static_cast<uint64_t>(newpos);
    io->pushback = -1;
    return 0;
}

static int wv_push_back_byte(void* id, int c)
{
    FMODWvIO* io = reinterpret_cast<FMODWvIO*>(id);
    io->pushback = c;
    if (io->pos > 0) io->pos--;
    return c;
}

static int64_t wv_get_length(void* id)
{
    return static_cast<int64_t>(reinterpret_cast<FMODWvIO*>(id)->length);
}

static int wv_can_seek(void* /*id*/) { return 1; }

static WavpackStreamReader64 g_wvReader = {
    wv_read,
    wv_write,
    wv_get_pos,
    wv_set_pos_abs,
    wv_set_pos_rel,
    wv_push_back_byte,
    wv_get_length,
    wv_can_seek,
    nullptr, // truncate_here (不要)
    nullptr  // close (不要)
};

// =========================================================
// PCM フォーマット情報
// =========================================================
struct PCMFormatInfo
{
    FMOD_SOUND_FORMAT fmodFormat;
    int               bytesPerSample;
};

static PCMFormatInfo resolveWvFormat(int bps, bool isFloat)
{
    if (isFloat)   return { FMOD_SOUND_FORMAT_PCMFLOAT, 4 };
    if (bps <= 16) return { FMOD_SOUND_FORMAT_PCM16,    2 };
    if (bps <= 24) return { FMOD_SOUND_FORMAT_PCM24,    3 };
    return               { FMOD_SOUND_FORMAT_PCM32,    4 };
}

// =========================================================
// サンプル変換ヘルパー
// WavpackUnpackSamples は常に int32_t で返す
// float 音声の場合は int32_t のビット列が IEEE 754 float
// =========================================================
static void convertSamples(const int32_t* src, uint8_t* dst,
                            int64_t totalSamples,  // channels 込みの総サンプル数
                            int bps, bool isFloat)
{
    if (isFloat || bps > 24)
    {
        memcpy(dst, src, static_cast<size_t>(totalSamples) * 4);
    }
    else if (bps <= 16)
    {
        int16_t* out = reinterpret_cast<int16_t*>(dst);
        for (int64_t i = 0; i < totalSamples; i++)
            out[i] = static_cast<int16_t>(src[i]);
    }
    else // 17-24 bit
    {
        for (int64_t i = 0; i < totalSamples; i++)
        {
            int32_t v = src[i];
            dst[i * 3 + 0] = static_cast<uint8_t>(v & 0xFF);
            dst[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            dst[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        }
    }
}

// =========================================================
// コーデック状態
// =========================================================
struct WvInfo
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
// タグ取得ヘルパー (WavPack ネイティブ APEv2)
// =========================================================
static std::string wvGetTag(WavpackContext* ctx, const char* key)
{
    char buf[512] = {};
    if (WavpackGetTagItem(ctx, key, buf, static_cast<int>(sizeof(buf))) > 0)
        return std::string(buf);
    return {};
}

// =========================================================
// open コールバック
// =========================================================
static FMOD_RESULT F_CALL wvCodec_open(FMOD_CODEC_STATE* codec, FMOD_MODE /*usermode*/,
                                        FMOD_CREATESOUNDEXINFO* /*userexinfo*/)
{
    if (!codec) return FMOD_ERR_INTERNAL;

    // ファイルサイズ取得
    unsigned int totalSize = 0;
    codec->functions->size(codec, &totalSize);
    if (totalSize < 4) return FMOD_ERR_FILE_EOF;

    // マジックバイト確認 ("wvpk")
    uint8_t      magic[4] = {};
    unsigned int rb       = 0;
    codec->functions->read(codec, magic, 4, &rb);
    if (rb < 4 || memcmp(magic, "wvpk", 4) != 0) return FMOD_ERR_FORMAT;
    codec->functions->seek(codec, 0, FMOD_CODEC_SEEK_METHOD_SET);

    // IO ブリッジ設定
    FMODWvIO io  = {};
    io.codec     = codec;
    io.pos       = 0;
    io.length    = static_cast<uint64_t>(totalSize);
    io.pushback  = -1;

    // WavPack コンテキストを開く
    char errMsg[128] = {};
    WavpackContext* ctx = WavpackOpenFileInputEx64(
        &g_wvReader, &io, nullptr, errMsg, OPEN_WVC, 0);
    if (!ctx) return FMOD_ERR_FORMAT;

    // ストリーム情報取得
    const int     nChannels  = WavpackGetNumChannels(ctx);
    const int     sampleRate = WavpackGetSampleRate(ctx);
    const int     bps        = WavpackGetBitsPerSample(ctx);
    const int     mode       = WavpackGetMode(ctx);
    const int64_t nFrames    = static_cast<int64_t>(WavpackGetNumSamples64(ctx));
    const bool    isFloat    = (mode & MODE_FLOAT) != 0;

    if (nFrames <= 0 || nChannels <= 0 || sampleRate <= 0)
    {
        WavpackCloseFile(ctx);
        return FMOD_ERR_FORMAT;
    }

    const auto fmt        = resolveWvFormat(bps, isFloat);
    const size_t totalBytes = static_cast<size_t>(nFrames) * nChannels * fmt.bytesPerSample;

    std::vector<uint8_t>  pcmBuffer(totalBytes);
    const uint32_t         CHUNK = 65536;
    std::vector<int32_t>   tmp(static_cast<size_t>(CHUNK) * nChannels);
    int64_t  decoded = 0;
    uint8_t* dst     = pcmBuffer.data();

    // =========================================================
    // 全フレームをデコード
    // =========================================================
    while (decoded < nFrames)
    {
        const uint32_t toRead = static_cast<uint32_t>(
            std::min(static_cast<int64_t>(CHUNK), nFrames - decoded));
        const uint32_t got = WavpackUnpackSamples(ctx, tmp.data(), toRead);
        if (got == 0) break;

        convertSamples(tmp.data(), dst,
                       static_cast<int64_t>(got) * nChannels,
                       bps, isFloat);
        dst     += static_cast<size_t>(got) * nChannels * fmt.bytesPerSample;
        decoded += got;
    }

    // =========================================================
    // タグ読み取り (APEv2 / ID3v1 を SDK が自動選択)
    // =========================================================
    const auto title  = wvGetTag(ctx, "Title");
    const auto artist = wvGetTag(ctx, "Artist");
    const auto album  = wvGetTag(ctx, "Album");

    WavpackCloseFile(ctx);

    // =========================================================
    // コーデック状態確定
    // =========================================================
    auto* x           = new WvInfo();
    x->buffer         = std::move(pcmBuffer);
    x->position       = 0;
    x->channels       = nChannels;
    x->sampleRate     = sampleRate;
    x->bytesPerSample = fmt.bytesPerSample;
    x->fmodFormat     = fmt.fmodFormat;

    x->waveFormat.format         = fmt.fmodFormat;
    x->waveFormat.channels       = nChannels;
    x->waveFormat.frequency      = sampleRate;
    x->waveFormat.pcmblocksize   = nChannels * fmt.bytesPerSample;
    x->waveFormat.lengthpcm      = static_cast<unsigned int>(nFrames); // PCMフレーム数（バイト数ではない）

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
static FMOD_RESULT F_CALL wvCodec_close(FMOD_CODEC_STATE* codec)
{
    delete reinterpret_cast<WvInfo*>(codec->plugindata);
    codec->plugindata = nullptr;
    return FMOD_OK;
}

// =========================================================
// read コールバック
// =========================================================
static FMOD_RESULT F_CALL wvCodec_read(FMOD_CODEC_STATE* codec,
    void* buffer, unsigned int sizebytes, unsigned int* bytesread)
{
    auto* x = reinterpret_cast<WvInfo*>(codec->plugindata);
    const uint64_t bufSize = static_cast<uint64_t>(x->buffer.size());
    if (x->position >= bufSize)
    {
        *bytesread = 0;
        return FMOD_ERR_FILE_EOF;
    }
    const uint64_t remaining = bufSize - x->position;
    const unsigned int toCopy = static_cast<unsigned int>(
        std::min(static_cast<uint64_t>(sizebytes * x->channels * x->bytesPerSample), remaining));
    memcpy(buffer, x->buffer.data() + x->position, toCopy);
    x->position += toCopy;
    *bytesread   = toCopy;
    return FMOD_OK;
}

// =========================================================
// その他コールバック
// =========================================================
static FMOD_RESULT F_CALL wvCodec_setposition(FMOD_CODEC_STATE* codec,
    int /*subsound*/, unsigned int position, FMOD_TIMEUNIT postype)
{
    auto* x = reinterpret_cast<WvInfo*>(codec->plugindata);
    if (postype == FMOD_TIMEUNIT_PCMBYTES)
        x->position = std::min(static_cast<uint64_t>(position),
                               static_cast<uint64_t>(x->buffer.size()));
    return FMOD_OK;
}

static FMOD_RESULT F_CALL wvCodec_getposition(FMOD_CODEC_STATE* codec,
    unsigned int* position, FMOD_TIMEUNIT postype)
{
    auto* x = reinterpret_cast<WvInfo*>(codec->plugindata);
    if (postype == FMOD_TIMEUNIT_PCMBYTES)
        *position = static_cast<unsigned int>(x->position);
    return FMOD_OK;
}

static FMOD_RESULT F_CALL wvCodec_getlength(FMOD_CODEC_STATE* codec,
    unsigned int* length, FMOD_TIMEUNIT lengthtype)
{
    auto* x = reinterpret_cast<WvInfo*>(codec->plugindata);
    if (lengthtype == FMOD_TIMEUNIT_PCMBYTES)
        *length = static_cast<unsigned int>(x->buffer.size());
    return FMOD_OK;
}

static FMOD_RESULT F_CALL wvCodec_soundcreated(FMOD_CODEC_STATE* codec, int subsound, FMOD_SOUND* sound)
{
    return FMOD_OK;
}

static FMOD_RESULT F_CALL wvCodec_getwaveformat(FMOD_CODEC_STATE* codec,
    int index, FMOD_CODEC_WAVEFORMAT* waveformat)
{
    if (index != 0) return FMOD_ERR_FORMAT;
    auto* x  = reinterpret_cast<WvInfo*>(codec->plugindata);
    *waveformat = x->waveFormat;
    return FMOD_OK;
}

// =========================================================
// プラグイン登録
// =========================================================
static FMOD_CODEC_DESCRIPTION s_wvCodecDesc =
{
    FMOD_CODEC_PLUGIN_VERSION,
    "FMOD WavPack Codec",
    0x00010000,
    1,
    FMOD_TIMEUNIT_PCMBYTES,
    &wvCodec_open,
    &wvCodec_close,
    &wvCodec_read,
    &wvCodec_getlength,
    &wvCodec_setposition,
    &wvCodec_getposition,
    &wvCodec_soundcreated,
    &wvCodec_getwaveformat
};

#ifdef __cplusplus
extern "C" {
#endif
    __declspec(dllexport) FMOD_CODEC_DESCRIPTION* F_API FMODGetCodecDescription()
    {
        return &s_wvCodecDesc;
    }
#ifdef __cplusplus
}
#endif

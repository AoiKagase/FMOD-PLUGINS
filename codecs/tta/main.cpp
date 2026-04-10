// main.cpp
// FMOD TTA (True Audio) コーデックプラグイン
//
// =========================================================
// 外部ライブラリ: libtta++ 2.2
// 入手先: https://sourceforge.net/projects/tta/files/tta++/
//
// 以下のファイルは deps/src/tta/ に配置されます:
//   libtta.h
//   libtta.cpp   ← プロジェクトに含めてコンパイル
//
// libfaad.libのような別途ビルドは不要です。
// ソースを直接プロジェクトに取り込みます。
// =========================================================

#include <memory>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cstring>

#pragma warning(push)
#pragma warning(disable:4244 4267 4996)
#include <libtta.h>
#pragma warning(pop)

#include "fmod.h"

// =========================================================
// PCMフォーマット情報
// =========================================================
struct TTAPCMFormat
{
    FMOD_SOUND_FORMAT fmodFormat;
    unsigned int      bytesPerSample;
};

// =========================================================
// コーデック状態
// =========================================================
struct ttainfo
{
    uint32_t               sample_rates;
    uint32_t               channels;
    TTAPCMFormat           format;
    std::vector<std::byte> buffer;
    unsigned long          bufferlen;
    uint64_t               lengthpcm;   // 総PCMフレーム数 (チャンネルあたり)
    uint32_t               position;    // バッファ内バイトオフセット

    std::vector<std::byte> title;
    std::vector<std::byte> artist;
    std::vector<std::byte> album;
};

static TTAPCMFormat resolveTTAFormat(int bps)
{
    if (bps <= 16) return { FMOD_SOUND_FORMAT_PCM16, 2 };
    if (bps <= 24) return { FMOD_SOUND_FORMAT_PCM24, 3 };
    return               { FMOD_SOUND_FORMAT_PCM32, 4 };
}

// =========================================================
// FMOD コールバック → libtta++ IO ブリッジ
// 構造体の先頭が TTA_io_callback と一致する必要がある (C-style継承)
// =========================================================
struct FMODTtaIO
{
    TTA_io_callback   cb;       // 先頭に配置すること (キャスト先)
    FMOD_CODEC_STATE* codec;
    uint64_t          position; // シーク追跡用
};

static TTAint32 CALLBACK tta_read_cb(TTA_io_callback* io, TTAuint8* buffer, TTAuint32 size)
{
    FMODTtaIO* fio = reinterpret_cast<FMODTtaIO*>(io);
    unsigned int rb = 0;
    fio->codec->functions->read(fio->codec, buffer, (unsigned int)size, &rb);
    fio->position += rb;
    return static_cast<TTAint32>(rb);
}

static TTAint64 tta_seek_cb(TTA_io_callback* io, TTAint64 offset)
{
    FMODTtaIO* fio = reinterpret_cast<FMODTtaIO*>(io);
    fio->codec->functions->seek(fio->codec,
        static_cast<unsigned int>(offset), FMOD_CODEC_SEEK_METHOD_SET);
    fio->position = static_cast<uint64_t>(offset);
    return offset;
}

// =========================================================
// ID3v2 synchsafe integer デコード
// =========================================================
static uint32_t id3_synchsafe(const uint8_t* b)
{
    return ((uint32_t)(b[0] & 0x7f) << 21) |
           ((uint32_t)(b[1] & 0x7f) << 14) |
           ((uint32_t)(b[2] & 0x7f) <<  7) |
            (uint32_t)(b[3] & 0x7f);
}

// =========================================================
// ID3v2 タグ読み取り (TTA はファイル先頭に ID3v2 を持てる)
// TIT2=TITLE, TPE1=ARTIST, TALB=ALBUM を抽出する
// =========================================================
static void readID3v2Tags(const uint8_t* data, uint32_t size, ttainfo* x)
{
    if (size < 10 || memcmp(data, "ID3", 3) != 0) return;

    const uint8_t  version    = data[3];
    const bool     extHeader  = (data[5] & 0x40) != 0;
    const uint32_t totalTagSz = id3_synchsafe(data + 6) + 10;
    if (totalTagSz > size) return;

    uint32_t pos = 10;

    // 拡張ヘッダをスキップ
    if (extHeader && pos + 4 <= totalTagSz)
    {
        uint32_t extSz;
        if (version == 4)
            extSz = id3_synchsafe(data + pos);
        else
            extSz = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos+1] << 16) |
                    ((uint32_t)data[pos+2] <<  8) |  (uint32_t)data[pos+3];
        pos += extSz;
    }

    while (pos + 10 <= totalTagSz && data[pos] != 0)
    {
        char id[5] = {};
        memcpy(id, data + pos, 4);

        uint32_t frameSize;
        if (version == 4)
            frameSize = id3_synchsafe(data + pos + 4);
        else
            frameSize = ((uint32_t)data[pos+4] << 24) | ((uint32_t)data[pos+5] << 16) |
                        ((uint32_t)data[pos+6] <<  8) |  (uint32_t)data[pos+7];

        pos += 10;
        if (pos + frameSize > totalTagSz) break;

        if (frameSize > 1)
        {
            // data[pos] = エンコーディングバイト (0=Latin1, 1=UTF-16, 3=UTF-8)
            const char*    val  = reinterpret_cast<const char*>(data + pos + 1);
            const uint32_t vlen = frameSize - 1;

            if      (strcmp(id, "TIT2") == 0)
                x->title.assign ((std::byte*)val, (std::byte*)val + vlen);
            else if (strcmp(id, "TPE1") == 0)
                x->artist.assign((std::byte*)val, (std::byte*)val + vlen);
            else if (strcmp(id, "TALB") == 0)
                x->album.assign ((std::byte*)val, (std::byte*)val + vlen);
        }
        pos += frameSize;
    }
}

// =========================================================
// openコールバック
// =========================================================
FMOD_RESULT F_CALL ttaCodec_open(FMOD_CODEC_STATE* codec, FMOD_MODE usermode,
                                   FMOD_CREATESOUNDEXINFO* userexinfo)
{
    if (!codec) return FMOD_ERR_INTERNAL;

    unsigned int totalSize = 0;
    codec->functions->size(codec, &totalSize);
    if (totalSize < 22) return FMOD_ERR_FILE_EOF; // TTA最小ヘッダサイズ

    // =========================================================
    // TTA ヘッダ検出
    // "TTA1" または "TTA2" の 4 バイトマジックを探す。
    // ID3v2 タグがファイル先頭にある場合はその後に TTA ヘッダが来る。
    // =========================================================
    uint8_t hdr4[4] = {};
    unsigned int rb = 0;
    codec->functions->read(codec, hdr4, 4, &rb);
    if (rb < 4) return FMOD_ERR_FORMAT;

    uint32_t ttaStart = 0;

    if (memcmp(hdr4, "ID3", 3) == 0)
    {
        // ID3v2 タグサイズを計算してスキップ
        uint8_t id3hdr[10] = {};
        codec->functions->seek(codec, 0, FMOD_CODEC_SEEK_METHOD_SET);
        codec->functions->read(codec, id3hdr, 10, &rb);
        if (rb < 10) return FMOD_ERR_FORMAT;
        ttaStart = id3_synchsafe(id3hdr + 6) + 10;
        codec->functions->seek(codec, ttaStart, FMOD_CODEC_SEEK_METHOD_SET);
        codec->functions->read(codec, hdr4, 4, &rb);
        if (rb < 4) return FMOD_ERR_FORMAT;
    }

    if (memcmp(hdr4, "TTA1", 4) != 0 && memcmp(hdr4, "TTA2", 4) != 0)
        return FMOD_ERR_FORMAT;

    // =========================================================
    // libtta++ IO ブリッジ設定
    // =========================================================
    FMODTtaIO fio    = {};
    fio.cb.read      = tta_read_cb;
    fio.cb.write     = nullptr;
    fio.cb.seek      = tta_seek_cb;
    fio.codec        = codec;
    fio.position     = ttaStart;

    // デコーダ生成位置にシーク
    codec->functions->seek(codec, ttaStart, FMOD_CODEC_SEEK_METHOD_SET);

    TTA_info  info = {};
    TTAuint32      decodedFrames = 0;
    TTAPCMFormat   fmt  = {};
    std::vector<std::byte> pcmBuffer;

    try
    {
        tta::tta_decoder decoder(&fio.cb);
        decoder.init_get_info(&info, static_cast<TTAint64>(ttaStart));

        fmt = resolveTTAFormat(info.bps);
        const uint64_t totalBytes =
            static_cast<uint64_t>(info.samples) * info.nch * fmt.bytesPerSample;
        pcmBuffer.resize(totalBytes);

        // 全フレームを一括デコード
        decodedFrames = decoder.process_stream(
            reinterpret_cast<TTAuint8*>(pcmBuffer.data()), info.samples);
    }
    catch (const tta::tta_exception& e)
    {
        return (e.code() == TTA_FORMAT_ERROR) ? FMOD_ERR_FORMAT : FMOD_ERR_FILE_BAD;
    }

    if (decodedFrames == 0) return FMOD_ERR_FORMAT;

    // =========================================================
    // ID3v2 タグ読み取り
    // =========================================================
    auto x = std::make_unique<ttainfo>();

    if (ttaStart > 0)
    {
        std::vector<uint8_t> id3data(ttaStart);
        codec->functions->seek(codec, 0, FMOD_CODEC_SEEK_METHOD_SET);
        codec->functions->read(codec, id3data.data(), ttaStart, &rb);
        if (rb == ttaStart)
            readID3v2Tags(id3data.data(), ttaStart, x.get());
    }

    if (!x->title.empty())
        codec->functions->metadata(codec, FMOD_TAGTYPE_ID3V2, (char*)"TITLE",
            x->title.data(), (unsigned int)x->title.size(), FMOD_TAGDATATYPE_STRING_UTF8, 1);
    if (!x->artist.empty())
        codec->functions->metadata(codec, FMOD_TAGTYPE_ID3V2, (char*)"ARTIST",
            x->artist.data(), (unsigned int)x->artist.size(), FMOD_TAGDATATYPE_STRING_UTF8, 1);
    if (!x->album.empty())
        codec->functions->metadata(codec, FMOD_TAGTYPE_ID3V2, (char*)"ALBUM",
            x->album.data(), (unsigned int)x->album.size(), FMOD_TAGDATATYPE_STRING_UTF8, 1);

    const unsigned long actualBytes = decodedFrames * info.nch * fmt.bytesPerSample;

    x->sample_rates = info.sps;
    x->channels     = info.nch;
    x->format       = fmt;
    x->bufferlen    = actualBytes;
    x->buffer       = std::move(pcmBuffer);
    x->buffer.resize(actualBytes);
    x->lengthpcm    = decodedFrames;
    x->position     = 0;

    codec->numsubsounds = 0;
    codec->plugindata   = x.release();
    return FMOD_OK;
}

// =========================================================
// closeコールバック
// =========================================================
FMOD_RESULT F_CALL ttaCodec_close(FMOD_CODEC_STATE* codec)
{
    if (codec->plugindata)
    {
        delete static_cast<ttainfo*>(codec->plugindata);
        codec->plugindata = nullptr;
    }
    return FMOD_OK;
}

// =========================================================
// readコールバック
// =========================================================
FMOD_RESULT F_CALL ttaCodec_read(FMOD_CODEC_STATE* codec, void* buffer,
                                   unsigned int sizebytes, unsigned int* bytesread)
{
    ttainfo* x = static_cast<ttainfo*>(codec->plugindata);
    if (!x || !bytesread) return FMOD_ERR_INTERNAL;

    const uint64_t remaining = x->bufferlen - x->position;
    if (remaining == 0)
    {
        *bytesread = 0;
        return FMOD_ERR_FILE_EOF;
    }

    const unsigned int toCopy = static_cast<unsigned int>(
        min(static_cast<uint64_t>(sizebytes * x->channels * x->format.bytesPerSample),
                 remaining));

    memset(buffer, 0, toCopy + 1);
    std::memcpy(buffer, &x->buffer[x->position], toCopy);
    x->position += toCopy;
    *bytesread   = toCopy;
    return FMOD_OK;
}

// =========================================================
// その他コールバック
// =========================================================
FMOD_RESULT F_CALL ttaCodec_getlength(FMOD_CODEC_STATE*, unsigned int*, FMOD_TIMEUNIT)
{
    return FMOD_OK;
}

FMOD_RESULT F_CALL ttaCodec_setposition(FMOD_CODEC_STATE* codec, int,
                                          unsigned int position, FMOD_TIMEUNIT)
{
    ttainfo* x = static_cast<ttainfo*>(codec->plugindata);
    if (!x) return FMOD_ERR_INTERNAL;
    x->position = position;
    return FMOD_OK;
}

FMOD_RESULT F_CALL ttaCodec_getposition(FMOD_CODEC_STATE* codec,
                                          unsigned int* position, FMOD_TIMEUNIT)
{
    ttainfo* x = static_cast<ttainfo*>(codec->plugindata);
    if (!x) return FMOD_ERR_INTERNAL;
    *position = x->position;
    return FMOD_OK;
}

FMOD_RESULT F_CALL ttaCodec_soundcreated(FMOD_CODEC_STATE*, int, FMOD_SOUND*)
{
    return FMOD_OK;
}

FMOD_RESULT F_CALL ttaCodec_getWaveFormat(FMOD_CODEC_STATE* codec, int,
                                            FMOD_CODEC_WAVEFORMAT* wf)
{
    ttainfo* x = static_cast<ttainfo*>(codec->plugindata);
    if (!x) return FMOD_ERR_INTERNAL;
    wf->channels  = static_cast<int>(x->channels);
    wf->format    = x->format.fmodFormat;
    wf->mode      = FMOD_DEFAULT;
    wf->frequency = static_cast<int>(x->sample_rates);
    wf->lengthpcm = static_cast<unsigned int>(x->lengthpcm);
    return FMOD_OK;
}

// =========================================================
// コーデック記述子
// =========================================================
FMOD_CODEC_DESCRIPTION ttaCodecDesc = {
    FMOD_CODEC_PLUGIN_VERSION,
    "FMOD TTA (True Audio) Codec",
    0x00010000,
    0,
    FMOD_TIMEUNIT_PCMBYTES,
    &ttaCodec_open,
    &ttaCodec_close,
    &ttaCodec_read,
    &ttaCodec_getlength,
    &ttaCodec_setposition,
    &ttaCodec_getposition,
    &ttaCodec_soundcreated,
    &ttaCodec_getWaveFormat
};

#ifdef __cplusplus
extern "C" {
#endif
    __declspec(dllexport) FMOD_CODEC_DESCRIPTION* F_API FMODGetCodecDescription()
    {
        return &ttaCodecDesc;
    }
#ifdef __cplusplus
}
#endif

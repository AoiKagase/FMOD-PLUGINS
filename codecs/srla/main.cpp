// main.cpp
// FMOD SRLA コーデックプラグイン
//
// =========================================================
// 外部ライブラリ: SRLA (Soleil Rising Lossless Audio Codec)
// 入手先: https://github.com/aikiriao/SRLA
// ライセンス: MIT License
//
// 必要なファイル:
//   deps/src/srla/include/srla.h
//   deps/src/srla/include/srla_decoder.h
//   deps/src/srla/include/srla_stdint.h
//   deps/lib/srla/x64/Release/srladec.lib
//   deps/lib/srla/x64/Release/srlacodec.lib
// =========================================================

#include <memory>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>
#include <cctype>
#include <cstdlib>

#include <srla_decoder.h>

#include "fmod.h"
#include "fmod_codec.h"

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
    std::vector<uint8_t>  title;
    std::vector<uint8_t>  artist;
    std::vector<uint8_t>  album;
    std::vector<uint8_t>  coverArt;
    std::string           year;
    std::string           trackNumber;
    std::string           trackTotal;
    uint64_t              position       = 0;
    int                   channels       = 0;
    int                   sampleRate     = 0;
    int                   bytesPerSample = 0;
    FMOD_SOUND_FORMAT     fmodFormat     = FMOD_SOUND_FORMAT_PCM16;
    FMOD_CODEC_WAVEFORMAT waveFormat     = {};
};

struct SRLAAPEv2Range
{
    uint32_t tagStart = 0;
    uint32_t tagEnd   = 0;
};

static uint32_t readLE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

static std::string toLowerASCII(std::string s)
{
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

static std::string trimASCII(std::string s)
{
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };

    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

static void splitTrackValue(const std::string& value, std::string& number, std::string& total)
{
    number.clear();
    total.clear();

    const size_t slash = value.find('/');
    if (slash == std::string::npos)
    {
        number = trimASCII(value);
        return;
    }

    number = trimASCII(value.substr(0, slash));
    total = trimASCII(value.substr(slash + 1));
}

static bool findTrailingAPEv2Range(const uint8_t* fileData, uint32_t fileSize, SRLAAPEv2Range& outRange)
{
    if (fileSize < 32) return false;

    const uint32_t footerOffsets[] = {
        fileSize - 32,
        (fileSize >= 160) ? (fileSize - 160) : 0
    };

    for (const uint32_t footerOffset : footerOffsets)
    {
        if (footerOffset + 32 > fileSize) continue;
        if (std::memcmp(fileData + footerOffset, "APETAGEX", 8) != 0) continue;

        const uint8_t* footer    = fileData + footerOffset;
        const uint32_t tagSize   = readLE32(footer + 12);
        const uint32_t itemCount = readLE32(footer + 16);
        const uint32_t flags     = readLE32(footer + 20);
        const uint32_t tagStart  = footerOffset + 32 - tagSize;
        const bool hasHeader     = ((flags >> 31) & 1u) != 0;

        if (itemCount == 0 || tagSize < 32 || tagStart > footerOffset) continue;
        if (tagStart > fileSize || footerOffset + 32 > fileSize) continue;
        if (hasHeader && tagSize < 64) continue;

        outRange.tagStart = tagStart;
        outRange.tagEnd   = footerOffset + 32;
        return true;
    }

    return false;
}

static void readTrailingAPEv2Tags(const uint8_t* fileData, uint32_t fileSize, SRLAInfo* x, uint32_t* audioDataSize)
{
    if (audioDataSize)
        *audioDataSize = fileSize;

    SRLAAPEv2Range tagRange = {};
    if (!findTrailingAPEv2Range(fileData, fileSize, tagRange))
        return;

    if (audioDataSize)
        *audioDataSize = tagRange.tagStart;

    const uint8_t* footer    = fileData + tagRange.tagEnd - 32;
    const uint32_t itemCount = readLE32(footer + 16);
    const uint32_t flags     = readLE32(footer + 20);
    const bool hasHeader     = ((flags >> 31) & 1u) != 0;

    const uint8_t* itemsStart = fileData + tagRange.tagStart + (hasHeader ? 32 : 0);
    const uint8_t* itemsEnd   = footer;
    const uint8_t* p          = itemsStart;
    uint32_t parsed           = 0;

    while (p + 8 <= itemsEnd && parsed < itemCount)
    {
        const uint32_t valueLen   = readLE32(p + 0);
        const uint32_t itemFlags  = readLE32(p + 4);
        const uint32_t valueType  = (itemFlags >> 1) & 0x3u;
        p += 8;

        const uint8_t* keyStart = p;
        while (p < itemsEnd && *p != 0) ++p;
        if (p >= itemsEnd) break;

        const std::string key(reinterpret_cast<const char*>(keyStart), p - keyStart);
        const std::string keyLower = toLowerASCII(key);
        ++p;

        if (p + valueLen > itemsEnd) break;
        const uint8_t* value = p;
        p += valueLen;
        ++parsed;

        if (keyLower == "title")
        {
            x->title.assign(value, value + valueLen);
        }
        else if (keyLower == "artist")
        {
            x->artist.assign(value, value + valueLen);
        }
        else if (keyLower == "album")
        {
            x->album.assign(value, value + valueLen);
        }
        else if (keyLower == "year" || keyLower == "date")
        {
            x->year.assign(reinterpret_cast<const char*>(value), valueLen);
        }
        else if (keyLower == "track")
        {
            const std::string trackValue(reinterpret_cast<const char*>(value), valueLen);
            splitTrackValue(trackValue, x->trackNumber, x->trackTotal);
        }
        else if (keyLower == "tracknumber")
        {
            x->trackNumber.assign(reinterpret_cast<const char*>(value), valueLen);
        }
        else if (keyLower == "tracktotal")
        {
            x->trackTotal.assign(reinterpret_cast<const char*>(value), valueLen);
        }
        else if ((keyLower == "cover art (front)" || keyLower == "cover art" || keyLower == "coverart")
            && valueType == 1 && valueLen > 0)
        {
            const void* descEnd = std::memchr(value, 0, valueLen);

            if (descEnd != nullptr)
            {
                const uint8_t* imageData = static_cast<const uint8_t*>(descEnd) + 1;
                if (imageData <= value + valueLen)
                {
                    x->coverArt.assign(imageData, value + valueLen);
                }
            }
            else
            {
                // SRLAEncoder 側は raw 画像 bytes をそのまま入れているのでこちらを採用する
                x->coverArt.assign(value, value + valueLen);
            }
        }
    }
}

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

    auto* x = new SRLAInfo();
    uint32_t audioDataSize = rb;
    readTrailingAPEv2Tags(fileData.data(), rb, x, &audioDataSize);
    if (audioDataSize < SRLA_HEADER_SIZE)
    {
        delete x;
        return FMOD_ERR_FORMAT;
    }

    std::vector<uint8_t> audioDataStorage;
    const uint8_t* audioData = fileData.data();
    if (audioDataSize != rb)
    {
        // Trailing metadata is parsed separately; hand the decoder a trimmed payload.
        audioDataStorage.assign(fileData.begin(), fileData.begin() + audioDataSize);
        audioData = audioDataStorage.data();
    }

    // ヘッダデコード（フォーマット検出）
    SRLAHeader header = {};
    if (SRLADecoder_DecodeHeader(audioData, audioDataSize, &header) != SRLA_APIRESULT_OK)
    {
        delete x;
        return FMOD_ERR_FORMAT;
    }

    if (header.num_channels == 0 || header.num_samples == 0 || header.sampling_rate == 0)
    {
        delete x;
        return FMOD_ERR_FORMAT;
    }

    // デコーダ設定
    SRLADecoderConfig config      = {};
    config.max_num_channels       = header.num_channels;
    config.max_num_parameters     = SRLA_MAX_COEFFICIENT_ORDER;
    config.check_checksum         = 1;

    const int32_t workSize = SRLADecoder_CalculateWorkSize(&config);
    if (workSize < 0)
    {
        delete x;
        return FMOD_ERR_INTERNAL;
    }

    std::vector<uint8_t> work(static_cast<size_t>(workSize));
    SRLADecoder* decoder = SRLADecoder_Create(&config, work.data(), workSize);
    if (!decoder)
    {
        delete x;
        return FMOD_ERR_INTERNAL;
    }

    if (SRLADecoder_SetHeader(decoder, &header) != SRLA_APIRESULT_OK)
    {
        SRLADecoder_Destroy(decoder);
        delete x;
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
        audioData, audioDataSize,
        chPtrs.data(), numCh, numSamples);

    SRLADecoder_Destroy(decoder);

    if (res != SRLA_APIRESULT_OK)
    {
        delete x;
        return FMOD_ERR_FILE_BAD;
    }

    // PCMフォーマット決定・インターリーブ変換
    const SRLAPCMFormat fmt = resolveSRLAFormat(header.bits_per_sample);
    const size_t totalBytes =
        static_cast<size_t>(numSamples) * numCh * fmt.bytesPerSample;

    std::vector<uint8_t> pcmBuffer(totalBytes);
    convertToInterleaved(chPtrs.data(), numCh, numSamples,
                         pcmBuffer.data(), fmt);

    // コーデック状態確定
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

    auto setTextTag = [&](const char* key, const std::vector<uint8_t>& value)
    {
        if (value.empty()) return;
        codec->functions->metadata(codec,
            FMOD_TAGTYPE_USER, const_cast<char*>(key),
            const_cast<uint8_t*>(value.data()),
            static_cast<unsigned int>(value.size()),
            FMOD_TAGDATATYPE_STRING_UTF8, 1);
    };

    setTextTag("TITLE",  x->title);
    setTextTag("ARTIST", x->artist);
    setTextTag("ALBUM",  x->album);

    auto setStringTag = [&](const char* key, const std::string& value)
    {
        if (value.empty()) return;
        codec->functions->metadata(codec,
            FMOD_TAGTYPE_USER, const_cast<char*>(key),
            const_cast<char*>(value.c_str()),
            static_cast<unsigned int>(value.size() + 1),
            FMOD_TAGDATATYPE_STRING, 1);
    };

    if (!x->year.empty())
    {
        setStringTag("YEAR", x->year);
        setStringTag("Year", x->year);
    }

    if (!x->trackNumber.empty())
    {
        setStringTag("TRACKNUMBER", x->trackNumber);
        setStringTag("TrackNumber", x->trackNumber);
    }

    if (!x->trackTotal.empty())
    {
        setStringTag("TRACKTOTAL", x->trackTotal);
        setStringTag("TrackTotal", x->trackTotal);
    }

    if (!x->trackNumber.empty())
    {
        std::string trackValue = x->trackNumber;
        if (!x->trackTotal.empty())
            trackValue += "/" + x->trackTotal;
        setStringTag("TRACK", trackValue);
        setStringTag("Track", trackValue);
    }

    if (!x->coverArt.empty())
    {
        codec->functions->metadata(codec,
            FMOD_TAGTYPE_USER, (char*)"COVERART",
            x->coverArt.data(),
            static_cast<unsigned int>(x->coverArt.size()),
            FMOD_TAGDATATYPE_BINARY, 1);
    }

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
    0x00010003,
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

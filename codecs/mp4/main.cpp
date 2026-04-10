#pragma comment(lib, "libfaad.lib")
#include <memory>
#include <iterator>
#include <vector>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <cstdint>
#include "neaacdec.h"
#include "fmod.h"

// Apple ALAC Decoder
// 以下のファイルは deps/src/mp4/alac/ へ自動展開されます。
// 入手先: https://github.com/macosforge/alac
//   ALACDecoder.h     ALACDecoder.cpp
//   ALACAudioTypes.h
//   ALACBitUtilities.h  ALACBitUtilities.c
//   ag_dec.c
//   dp_dec.c
//   matrix_dec.c
//   EndianPortable.h    EndianPortable.c
#include <ALACDecoder.h>
#include <ALACAudioTypes.h>
#include <ALACBitUtilities.h>

#pragma warning(push)
#pragma warning(disable:4018)
#pragma warning(disable:4101)
#pragma warning(disable:4267)
#define MINIMP4_IMPLEMENTATION
#include <minimp4.h>
#pragma warning(pop)

typedef unsigned long       DWORD;
typedef int                 BOOL;
typedef unsigned char       BYTE;
typedef unsigned short      WORD;

// =========================================================
// コーデック種別
// =========================================================
enum CodecType { CODEC_AAC, CODEC_ALAC };

typedef struct
{
    std::byte size[4];
    std::byte header[4];
    std::vector<std::byte> data;
} MP4HEADER;

// FAAD2とFMODのフォーマット対応情報
typedef struct
{
    FMOD_SOUND_FORMAT  fmodFormat;
    unsigned char      faadFormat;      // FAAD2用 (ALAC時は未使用)
    unsigned int       bytesPerSample;
} PCMFormatInfo;

typedef struct
{
    CodecType      codecType;
    std::uint64_t  sample_rates;
    std::byte      channels;
    NeAACDecHandle aac;                 // AACデコーダハンドル (ALAC時はnullptr)
    std::vector<std::byte> buffer;
    unsigned long  bufferlen;

    std::uint64_t  lengthpcm;
    std::uint32_t  position;

    PCMFormatInfo  format;

    std::vector<std::byte> title;
    std::vector<std::byte> artist;
    std::vector<std::byte> album;
} info;

// =========================================================
// フォーマット解決 (AAC用: userexinfoの指定に従う)
// =========================================================
PCMFormatInfo resolvePCMFormat(const FMOD_CREATESOUNDEXINFO* userexinfo)
{
    FMOD_SOUND_FORMAT requestedFormat = FMOD_SOUND_FORMAT_NONE;
    if (userexinfo)
        requestedFormat = userexinfo->format;

    switch (requestedFormat)
    {
    case FMOD_SOUND_FORMAT_PCMFLOAT:
        return { FMOD_SOUND_FORMAT_PCMFLOAT, FAAD_FMT_FLOAT, 4 };
    case FMOD_SOUND_FORMAT_PCM32:
        return { FMOD_SOUND_FORMAT_PCM32, FAAD_FMT_32BIT, 4 };
    case FMOD_SOUND_FORMAT_PCM24:
        return { FMOD_SOUND_FORMAT_PCM24, FAAD_FMT_24BIT, 3 };
    case FMOD_SOUND_FORMAT_PCM16:
    default:
        return { FMOD_SOUND_FORMAT_PCM16, FAAD_FMT_16BIT, 2 };
    }
}

// =========================================================
// フォーマット解決 (ALAC用: ソースのビット深度をそのまま使用)
// =========================================================
PCMFormatInfo resolveALACFormat(uint8_t bitDepth)
{
    if (bitDepth <= 16) return { FMOD_SOUND_FORMAT_PCM16, 0, 2 };
    if (bitDepth <= 24) return { FMOD_SOUND_FORMAT_PCM24, 0, 3 };
    return                     { FMOD_SOUND_FORMAT_PCM32, 0, 4 };
}

std::uint32_t _get_size(const std::byte* size)
{
    std::uint32_t x = 0;
    for (size_t i = 0; i < sizeof(std::uint32_t); i++)
    {
        const std::uint8_t bit_shifts = static_cast<std::uint8_t>((sizeof(std::uint32_t) - 1 - i) * 8);
        x |= (std::uint32_t)size[i] << bit_shifts;
    }
    return x;
}

std::uint64_t _get_size_64(const std::byte* size)
{
    std::uint64_t x = 0;
    for (size_t i = 0; i < sizeof(std::uint64_t); i++)
    {
        const std::uint8_t bit_shifts = static_cast<std::uint8_t>((sizeof(std::uint64_t) - 1 - i) * 8);
        x |= (std::uint64_t)size[i] << bit_shifts;
    }
    return x;
}

// minimp4がFMODのファイルコールバック経由でデータを読み込むためのブリッジ関数
static int mp4_read_callback(int64_t offset, void* buffer, size_t size, void* token)
{
    FMOD_CODEC_STATE* codec = (FMOD_CODEC_STATE*)token;
    unsigned int bytesread = 0;
    codec->functions->seek(codec, (unsigned int)offset, FMOD_CODEC_SEEK_METHOD_SET);
    codec->functions->read(codec, buffer, (unsigned int)size, &bytesread);
    return (bytesread != size) ? 1 : 0;
}

// =========================================================
// MP4ボックス探索ヘルパー
// 指定範囲内で指定名のボックスを線形探索する。
//   out_data_start : ボックスデータ開始オフセット (8バイトヘッダの直後)
//   out_data_size  : ボックスデータサイズ (ヘッダを除く)
//   out_box_end    : ボックス全体の終端オフセット (次ボックスの先頭)
// =========================================================
static bool find_box(FMOD_CODEC_STATE* codec,
                     uint64_t search_start, uint64_t search_end,
                     const char* name,
                     uint64_t& out_data_start,
                     uint64_t& out_data_size,
                     uint64_t& out_box_end)
{
    uint64_t pos = search_start;
    while (pos + 8 <= search_end)
    {
        codec->functions->seek(codec, (unsigned int)pos, FMOD_CODEC_SEEK_METHOD_SET);
        uint8_t hdr[8];
        unsigned int rb = 0;
        codec->functions->read(codec, hdr, 8, &rb);
        if (rb < 8) return false;

        uint32_t sz = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                      ((uint32_t)hdr[2] <<  8) |  (uint32_t)hdr[3];
        if (sz < 8 || pos + sz > search_end) return false;

        if (memcmp(hdr + 4, name, 4) == 0)
        {
            out_data_start = pos + 8;
            out_data_size  = sz - 8;
            out_box_end    = pos + sz;
            return true;
        }
        pos += sz;
    }
    return false;
}

// =========================================================
// stsc/stco/stsz を直接読み取りフレームオフセットを計算する構造体
// minimp4 の payload_bytes バグで各テーブルが全ゼロになる問題を回避する
struct AlacFrameTable
{
    std::vector<uint32_t>  entrySizes;   // stsz: per-sample byte size
    std::vector<uint64_t>  chunkOffsets; // stco/co64: per-chunk file offset
    std::vector<uint32_t>  stscFirst;    // stsc: first_chunk (1-origin)
    std::vector<uint32_t>  stscSpc;      // stsc: samples_per_chunk

    // サンプル s のファイルオフセットとバイトサイズを返す
    bool frame_offset(uint32_t s, uint64_t& out_offset, uint32_t& out_bytes) const
    {
        if (s >= entrySizes.size() || chunkOffsets.empty()) return false;
        out_bytes = entrySizes[s];

        // stsc を使ってサンプル s がどのチャンクに属するかを求める
        const uint32_t nChunks = (uint32_t)chunkOffsets.size();
        uint32_t chunkIdx = 0;
        uint32_t sampleBase = 0;  // このチャンクの先頭サンプル番号
        uint32_t spc = stscSpc.empty() ? 1 : stscSpc[0];
        uint32_t stscGroupIdx = 0;

        for (uint32_t nc = 0; nc < nChunks; nc++)
        {
            // 次の stsc グループに移る? (first_chunk は 1-origin、nc は 0-origin)
            if (stscGroupIdx + 1 < (uint32_t)stscFirst.size() &&
                nc == stscFirst[stscGroupIdx + 1] - 1)  // 0-origin nc == 1-origin first_chunk - 1
            {
                stscGroupIdx++;
                spc = stscSpc[stscGroupIdx];
            }

            if (s < sampleBase + spc)
            {
                // サンプル s はチャンク nc の中にある
                uint32_t posInChunk = s - sampleBase;
                uint64_t byteOff = 0;
                for (uint32_t k = sampleBase; k < sampleBase + posInChunk; k++)
                    byteOff += entrySizes[k];
                out_offset = chunkOffsets[nc] + byteOff;
                return true;
            }
            sampleBase += spc;
        }
        return false;
    }
};

// =========================================================
// stsz / stco / stsc をファイルから直接読み取る
// MP4D_frame_offset に依存しないフレームテーブルを構築する
// =========================================================
static bool build_alac_frame_table(FMOD_CODEC_STATE* codec, uint32_t totalSize,
                                    const MP4D_demux_t& mp4, unsigned int trackIdx,
                                    AlacFrameTable& out)
{
    uint64_t ds, dz, be;
    if (!find_box(codec, 0, totalSize, "moov", ds, dz, be)) return false;
    const uint64_t moov_data = ds, moov_end = be;

    uint64_t trak_search = moov_data;
    uint64_t trak_data = 0, trak_end = 0;
    for (unsigned int t = 0; ; t++)
    {
        if (!find_box(codec, trak_search, moov_end, "trak", ds, dz, be)) return false;
        trak_search = be;
        if (t == trackIdx) { trak_data = ds; trak_end = be; break; }
    }

    uint64_t mdia_ds, mdia_dz, mdia_be;
    if (!find_box(codec, trak_data, trak_end, "mdia", mdia_ds, mdia_dz, mdia_be)) return false;
    uint64_t minf_ds, minf_dz, minf_be;
    if (!find_box(codec, mdia_ds, mdia_be, "minf", minf_ds, minf_dz, minf_be)) return false;
    uint64_t stbl_ds, stbl_dz, stbl_be;
    if (!find_box(codec, minf_ds, minf_be, "stbl", stbl_ds, stbl_dz, stbl_be)) return false;

    // --- stsz ---
    {
        uint64_t stsz_ds, stsz_dz, stsz_be;
        if (!find_box(codec, stbl_ds, stbl_be, "stsz", stsz_ds, stsz_dz, stsz_be) || stsz_dz < 12)
            return false;

        codec->functions->seek(codec, (unsigned int)stsz_ds, FMOD_CODEC_SEEK_METHOD_SET);
        uint8_t hdr[12]; unsigned int rb = 0;
        codec->functions->read(codec, hdr, 12, &rb);
        if (rb < 12) return false;

        const uint32_t sampleSize  = ((uint32_t)hdr[4]<<24)|((uint32_t)hdr[5]<<16)|
                                      ((uint32_t)hdr[6]<< 8)| (uint32_t)hdr[7];
        const uint32_t sampleCount = ((uint32_t)hdr[8]<<24)|((uint32_t)hdr[9]<<16)|
                                      ((uint32_t)hdr[10]<<8)| (uint32_t)hdr[11];
        if (sampleCount == 0) return false;

        out.entrySizes.resize(sampleCount);
        if (sampleSize != 0)
        {
            std::fill(out.entrySizes.begin(), out.entrySizes.end(), sampleSize);
        }
        else
        {
            std::vector<uint8_t> eb(sampleCount * 4);
            unsigned int rbE = 0;
            codec->functions->read(codec, eb.data(), sampleCount * 4, &rbE);
            if (rbE < sampleCount * 4) return false;
            for (uint32_t i = 0; i < sampleCount; i++)
                out.entrySizes[i] = ((uint32_t)eb[i*4]<<24)|((uint32_t)eb[i*4+1]<<16)|
                                    ((uint32_t)eb[i*4+2]<<8)| (uint32_t)eb[i*4+3];
        }
    }

    // --- stco / co64 ---
    {
        uint64_t stco_ds, stco_dz, stco_be;
        bool is64 = false;
        if (!find_box(codec, stbl_ds, stbl_be, "stco", stco_ds, stco_dz, stco_be))
        {
            if (!find_box(codec, stbl_ds, stbl_be, "co64", stco_ds, stco_dz, stco_be))
                return false;
            is64 = true;
        }
        if (stco_dz < 8) return false;

        codec->functions->seek(codec, (unsigned int)stco_ds, FMOD_CODEC_SEEK_METHOD_SET);
        uint8_t hdr[8]; unsigned int rb = 0;
        codec->functions->read(codec, hdr, 8, &rb);
        if (rb < 8) return false;
        const uint32_t entryCount = ((uint32_t)hdr[4]<<24)|((uint32_t)hdr[5]<<16)|
                                     ((uint32_t)hdr[6]<< 8)| (uint32_t)hdr[7];
        if (entryCount == 0) return false;

        out.chunkOffsets.resize(entryCount);
        const uint32_t entryBytes = is64 ? 8 : 4;
        std::vector<uint8_t> eb(entryCount * entryBytes);
        unsigned int rbE = 0;
        codec->functions->read(codec, eb.data(), entryCount * entryBytes, &rbE);
        if (rbE < entryCount * entryBytes) return false;
        for (uint32_t i = 0; i < entryCount; i++)
        {
            if (is64)
                out.chunkOffsets[i] = ((uint64_t)eb[i*8]<<56)|((uint64_t)eb[i*8+1]<<48)|
                                       ((uint64_t)eb[i*8+2]<<40)|((uint64_t)eb[i*8+3]<<32)|
                                       ((uint64_t)eb[i*8+4]<<24)|((uint64_t)eb[i*8+5]<<16)|
                                       ((uint64_t)eb[i*8+6]<<8) | (uint64_t)eb[i*8+7];
            else
                out.chunkOffsets[i] = ((uint32_t)eb[i*4]<<24)|((uint32_t)eb[i*4+1]<<16)|
                                       ((uint32_t)eb[i*4+2]<<8)| (uint32_t)eb[i*4+3];
        }
    }

    // --- stsc ---
    {
        uint64_t stsc_ds, stsc_dz, stsc_be;
        if (!find_box(codec, stbl_ds, stbl_be, "stsc", stsc_ds, stsc_dz, stsc_be) || stsc_dz < 8)
            return false;

        codec->functions->seek(codec, (unsigned int)stsc_ds, FMOD_CODEC_SEEK_METHOD_SET);
        uint8_t hdr[8]; unsigned int rb = 0;
        codec->functions->read(codec, hdr, 8, &rb);
        if (rb < 8) return false;
        const uint32_t entryCount = ((uint32_t)hdr[4]<<24)|((uint32_t)hdr[5]<<16)|
                                     ((uint32_t)hdr[6]<< 8)| (uint32_t)hdr[7];
        if (entryCount == 0) return false;

        out.stscFirst.resize(entryCount);
        out.stscSpc.resize(entryCount);
        std::vector<uint8_t> eb(entryCount * 12);  // first_chunk(4)+spc(4)+sdi(4)
        unsigned int rbE = 0;
        codec->functions->read(codec, eb.data(), entryCount * 12, &rbE);
        if (rbE < entryCount * 12) return false;
        for (uint32_t i = 0; i < entryCount; i++)
        {
            out.stscFirst[i] = ((uint32_t)eb[i*12]<<24)|((uint32_t)eb[i*12+1]<<16)|
                                ((uint32_t)eb[i*12+2]<<8)| (uint32_t)eb[i*12+3];
            out.stscSpc[i]   = ((uint32_t)eb[i*12+4]<<24)|((uint32_t)eb[i*12+5]<<16)|
                                ((uint32_t)eb[i*12+6]<<8)| (uint32_t)eb[i*12+7];
        }
    }

    return true;
}

// =========================================================
// ALACのMagic Cookie (ALACSpecificConfig 24バイト) をファイルから読み取る
// 探索パス: moov > trak > mdia > minf > stbl > stsd > alac(outer) > alac(inner)
// 成功時は outCookie[24] にビッグエンディアンの生バイトを格納して true を返す
// =========================================================
static bool read_alac_magic_cookie(FMOD_CODEC_STATE* codec, uint32_t totalSize,
                                    uint8_t outCookie[24])
{
    uint64_t ds, dz, be;

    // moov
    if (!find_box(codec, 0, totalSize, "moov", ds, dz, be)) return false;
    const uint64_t moov_data = ds;
    const uint64_t moov_end  = be;

    // 複数のtrakを順番に確認し、ALACオーディオトラックを探す
    uint64_t trak_search = moov_data;
    while (find_box(codec, trak_search, moov_end, "trak", ds, dz, be))
    {
        const uint64_t trak_data = ds;
        const uint64_t trak_end  = be;
        trak_search = be; // 次の trak 探索はこのボックスの後から

        uint64_t mdia_ds, mdia_dz, mdia_be;
        if (!find_box(codec, trak_data, trak_end, "mdia", mdia_ds, mdia_dz, mdia_be)) continue;

        uint64_t minf_ds, minf_dz, minf_be;
        if (!find_box(codec, mdia_ds, mdia_be, "minf", minf_ds, minf_dz, minf_be)) continue;

        uint64_t stbl_ds, stbl_dz, stbl_be;
        if (!find_box(codec, minf_ds, minf_be, "stbl", stbl_ds, stbl_dz, stbl_be)) continue;

        // stsd は FullBox: version(1) + flags(3) + entry_count(4) = 8バイト先頭をスキップ
        uint64_t stsd_ds, stsd_dz, stsd_be;
        if (!find_box(codec, stbl_ds, stbl_be, "stsd", stsd_ds, stsd_dz, stsd_be)) continue;

        // outer alac SampleEntry を探す (entry_count 8バイト分をスキップした位置から)
        uint64_t alac_ds, alac_dz, alac_be;
        if (!find_box(codec, stsd_ds + 8, stsd_be, "alac", alac_ds, alac_dz, alac_be)) continue;
        if (alac_dz < 36) continue;

        // outer alac ボックスの内容を全てメモリに読み込み、inner alac ボックスをスキャン
        // AudioSampleEntry は V0(28バイト)/V1(44バイト)/V2(72バイト) があるためオフセット固定不可
        // inner box: size(4) + "alac"(4) + version(1)+flags(3) + ALACSpecificConfig(24) = 合計36バイト
        std::vector<uint8_t> outerBuf(static_cast<size_t>(alac_dz));
        codec->functions->seek(codec, (unsigned int)alac_ds, FMOD_CODEC_SEEK_METHOD_SET);
        unsigned int rb = 0;
        codec->functions->read(codec, outerBuf.data(), (unsigned int)alac_dz, &rb);
        if (rb < 36) continue;

        bool found = false;
        // SampleEntry 基本8バイト以降をスキャン
        for (size_t i = 8; i + 36 <= (size_t)rb; i++)
        {
            if (outerBuf[i+4] == 'a' && outerBuf[i+5] == 'l' &&
                outerBuf[i+6] == 'a' && outerBuf[i+7] == 'c')
            {
                const uint32_t boxSz = ((uint32_t)outerBuf[i]   << 24) |
                                       ((uint32_t)outerBuf[i+1] << 16) |
                                       ((uint32_t)outerBuf[i+2] <<  8) |
                                        (uint32_t)outerBuf[i+3];
                if (boxSz >= 36 && i + boxSz <= (size_t)rb)
                {
                    // FullBox: version(1)+flags(3) の4バイト後に ALACSpecificConfig(24バイト)
                    std::memcpy(outCookie, outerBuf.data() + i + 12, 24);
                    found = true;
                    break;
                }
            }
        }
        if (found) return true;
    }
    return false;
}

// =========================================================
// openコールバック
// =========================================================
FMOD_RESULT F_CALL myCodec_open(FMOD_CODEC_STATE* codec, FMOD_MODE usermode, FMOD_CREATESOUNDEXINFO* userexinfo)
{
    if (!codec)
        return FMOD_ERR_INTERNAL;

    std::uint32_t totalSize = 0;
    codec->functions->size(codec, &totalSize);
    if (totalSize <= 0)
        return FMOD_ERR_FILE_EOF;

    // =========================================================
    // ftyp チェック (M4A / mp42 / alac)
    // =========================================================
    auto chunk = std::make_unique<MP4HEADER>();
    unsigned int readBytes = 0;
    FMOD_RESULT r;

    r = codec->functions->read(codec, chunk->size,   4, &readBytes);
    r = codec->functions->read(codec, chunk->header, 4, &readBytes);
    if (std::memcmp(chunk->header, "ftyp", 4) != 0)
        return FMOD_ERR_FORMAT;

    const std::uint64_t ftypSize = _get_size(chunk->size);
    chunk->data.resize(static_cast<size_t>(ftypSize - 8));
    r = codec->functions->read(codec, chunk->data.data(), static_cast<unsigned int>(ftypSize - 8), &readBytes);

    if (std::memcmp(chunk->data.data(), "M4A ", 4) != 0
        && std::memcmp(chunk->data.data(), "mp42", 4) != 0
        && std::memcmp(chunk->data.data(), "alac", 4) != 0)
    {
        return FMOD_ERR_FORMAT;
    }

    // =========================================================
    // minimp4 でファイル全体を解析 (サンプルテーブル + メタデータ)
    // =========================================================
    codec->functions->seek(codec, 0, FMOD_CODEC_SEEK_METHOD_SET);
    MP4D_demux_t mp4 = {};
    MP4D_open(&mp4, mp4_read_callback, codec, totalSize);

    // =========================================================
    // メタデータ取得
    // =========================================================
    auto x = std::make_unique<info>();
    x->aac = nullptr;

    if (mp4.tag.title)
    {
        const char* val = (const char*)mp4.tag.title;
        x->title.assign((std::byte*)val, (std::byte*)val + strlen(val));
        codec->functions->metadata(codec, FMOD_TAGTYPE_ID3V2, (char*)"TITLE",
            x->title.data(), static_cast<unsigned int>(x->title.size()), FMOD_TAGDATATYPE_STRING_UTF8, 1);
    }
    if (mp4.tag.artist)
    {
        const char* val = (const char*)mp4.tag.artist;
        x->artist.assign((std::byte*)val, (std::byte*)val + strlen(val));
        codec->functions->metadata(codec, FMOD_TAGTYPE_ID3V2, (char*)"ARTIST",
            x->artist.data(), static_cast<unsigned int>(x->artist.size()), FMOD_TAGDATATYPE_STRING_UTF8, 1);
    }
    if (mp4.tag.album)
    {
        const char* val = (const char*)mp4.tag.album;
        x->album.assign((std::byte*)val, (std::byte*)val + strlen(val));
        codec->functions->metadata(codec, FMOD_TAGTYPE_ID3V2, (char*)"ALBUM",
            x->album.data(), static_cast<unsigned int>(x->album.size()), FMOD_TAGDATATYPE_STRING_UTF8, 1);
    }

    // =========================================================
    // コーデック検出: alac ボックスが見つかれば ALAC、なければ AAC
    // =========================================================
    uint8_t alacCookie[24] = {};
    const bool isALAC = read_alac_magic_cookie(codec, totalSize, alacCookie);

    if (isALAC)
    {
        // =========================================================
        // ALACデコードパス
        // =========================================================
        x->codecType = CODEC_ALAC;

        // オーディオトラック番号を探す
        int audioTrack = -1;
        for (unsigned int i = 0; i < mp4.track_count; i++)
        {
            if (mp4.track[i].handler_type == MP4D_HANDLER_TYPE_SOUN)
            {
                audioTrack = (int)i;
                break;
            }
        }

        if (audioTrack < 0)
        {
            MP4D_close(&mp4);
            return FMOD_ERR_FORMAT;
        }

        // stsz/stco/stsc をファイルから直接読み取ってフレームテーブルを構築
        // (minimp4 の payload_bytes バグで各テーブルが全ゼロになる問題を回避)
        AlacFrameTable frameTable;
        const bool tableOk = build_alac_frame_table(codec, totalSize, mp4, (unsigned int)audioTrack, frameTable);
        if (!tableOk)
        {
            MP4D_close(&mp4);
            return FMOD_ERR_FORMAT;
        }

        // ALACデコーダ初期化
        // Init() にはファイルから読んだビッグエンディアン生バイト(24バイト)を渡す。
        // 内部でバイトスワップ済みの設定が mConfig に格納される。
        ALACDecoder alacDecoder;
        if (alacDecoder.Init(alacCookie, 24) != 0)
        {
            MP4D_close(&mp4);
            return FMOD_ERR_INTERNAL;
        }

        const ALACSpecificConfig& cfg = alacDecoder.mConfig;

        x->sample_rates = cfg.sampleRate;
        x->channels     = (std::byte)cfg.numChannels;
        x->format       = resolveALACFormat(cfg.bitDepth);

        const uint32_t bytesPerSample  = x->format.bytesPerSample;
        // 1フレームの最大出力バイト数 (最終フレームはこれより短い場合がある)
        const uint32_t maxFrameOutBytes = cfg.frameLength * cfg.numChannels * bytesPerSample;

        std::vector<uint8_t> frameIn;
        std::vector<uint8_t> frameOut(maxFrameOutBytes);
        std::vector<std::byte> decoded;

        const uint32_t sampleCount = (uint32_t)frameTable.entrySizes.size();

        for (uint32_t s = 0; s < sampleCount; s++)
        {
            uint64_t offset = 0;
            uint32_t frameBytes = 0;

            // フレームテーブルからファイルオフセットとサイズを取得
            if (!frameTable.frame_offset(s, offset, frameBytes)) continue;
            if (frameBytes == 0) continue;

            // 圧縮フレームの読み込み
            frameIn.resize(frameBytes);
            codec->functions->seek(codec, (unsigned int)offset, FMOD_CODEC_SEEK_METHOD_SET);
            unsigned int rb = 0;
            codec->functions->read(codec, frameIn.data(), frameBytes, &rb);
            if (rb < frameBytes) break;

            // ALACデコード (1フレーム分のPCMを frameOut に書き込む)
            // 圧縮フレームを BitBuffer にラップして Decode に渡す
            BitBuffer bits;
            BitBufferInit(&bits, frameIn.data(), frameBytes);

            uint32_t numSamples = 0;
            if (alacDecoder.Decode(&bits, frameOut.data(), cfg.frameLength, cfg.numChannels, &numSamples) != 0)
                break;

            // 実際にデコードされたバイト数 (最終フレームは frameLength より短い場合がある)
            const uint32_t actualBytes = numSamples * cfg.numChannels * bytesPerSample;
            const size_t   prevSize    = decoded.size();
            decoded.resize(prevSize + actualBytes);
            std::memcpy(decoded.data() + prevSize, frameOut.data(), actualBytes);
        }

        if (decoded.empty())
        {
            MP4D_close(&mp4);
            return FMOD_ERR_FORMAT;
        }

        x->bufferlen = static_cast<unsigned long>(decoded.size());
        x->buffer.assign(reinterpret_cast<std::byte*>(decoded.data()),
                          reinterpret_cast<std::byte*>(decoded.data()) + decoded.size());
        // lengthpcm = 総バイト数 ÷ チャンネル数 ÷ 1サンプルあたりのバイト数
        x->lengthpcm = x->bufferlen / bytesPerSample / cfg.numChannels;
    }
    else
    {
        // =========================================================
        // AACデコードパス (既存ロジック)
        // =========================================================
        x->codecType = CODEC_AAC;
        x->format    = resolvePCMFormat(userexinfo);

        // mdat ボックスをファイルから探して読み込む
        uint64_t mdat_ds, mdat_dz, mdat_be;
        if (!find_box(codec, 0, totalSize, "mdat", mdat_ds, mdat_dz, mdat_be))
        {
            MP4D_close(&mp4);
            return FMOD_ERR_FORMAT;
        }

        x->bufferlen = static_cast<unsigned long>(mdat_dz);
        x->buffer.resize(x->bufferlen);
        codec->functions->seek(codec, (unsigned int)mdat_ds, FMOD_CODEC_SEEK_METHOD_SET);
        unsigned int rb = 0;
        codec->functions->read(codec, x->buffer.data(), x->bufferlen, &rb);
        x->bufferlen = rb;

        // FAAD2デコーダオープン
        if (!(x->aac = NeAACDecOpen()))
        {
            MP4D_close(&mp4);
            return FMOD_ERR_INTERNAL;
        }

        if (NeAACDecInit(x->aac,
                         reinterpret_cast<unsigned char*>(x->buffer.data()), x->bufferlen,
                         reinterpret_cast<unsigned long*>(&x->sample_rates),
                         reinterpret_cast<unsigned char*>(&x->channels)) != 0)
        {
            NeAACDecClose(x->aac);
            x->aac = nullptr;
            MP4D_close(&mp4);
            return FMOD_ERR_INTERNAL;
        }

        // FAAD2コンフィグ
        NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(x->aac);
        config->outputFormat = x->format.faadFormat;
        if (userexinfo && userexinfo->defaultfrequency > 0)
            config->defSampleRate = userexinfo->defaultfrequency;
        else
            config->defSampleRate = static_cast<unsigned long>(x->sample_rates);
        NeAACDecSetConfiguration(x->aac, config);

        void* buf = nullptr;
        unsigned long position = 0;
        std::uint64_t read = 0;
        std::vector<std::byte> decoded;
        NeAACDecFrameInfo frameInfo;

        while (position < x->bufferlen)
        {
            buf = NeAACDecDecode(x->aac, &frameInfo,
                                  (unsigned char*)&x->buffer[position], x->bufferlen - position);

            if (frameInfo.error != 0)
            {
                x->bufferlen = 0;
                MP4D_close(&mp4);
                return FMOD_ERR_FILE_BAD;
            }

            if (frameInfo.bytesconsumed > x->bufferlen)
            {
                x->bufferlen = 0;
            }
            else
            {
                if (frameInfo.samples != 0)
                {
                    if (!buf)
                    {
                        MP4D_close(&mp4);
                        return FMOD_ERR_INTERNAL;
                    }
                    if (frameInfo.bytesconsumed > 0)
                    {
                        const std::uint64_t frameBytes = frameInfo.samples * x->format.bytesPerSample;
                        decoded.resize(decoded.size() + frameBytes);
                        std::memcpy(&decoded[read], buf, frameBytes);
                        position += frameInfo.bytesconsumed;
                        read += frameBytes;
                    }
                }
            }
        }

        x->bufferlen = static_cast<unsigned long>(read);
        x->lengthpcm = static_cast<std::uint32_t>(read / x->format.bytesPerSample / static_cast<unsigned int>(x->channels));

        x->buffer.clear();
        x->buffer.resize(read);
        std::memcpy(x->buffer.data(), decoded.data(), read);
    }

    MP4D_close(&mp4);

    codec->numsubsounds = 0;
    codec->plugindata   = x.release();

    return FMOD_OK;
}

// closeコールバック
FMOD_RESULT F_CALL myCodec_close(FMOD_CODEC_STATE* codec)
{
    if (codec->plugindata != nullptr)
    {
        info* x = (info*)codec->plugindata;
        if (x->aac)
            NeAACDecClose(x->aac);
        delete(x);
    }
    return FMOD_OK;
}

// readコールバック
FMOD_RESULT F_CALL myCodec_read(FMOD_CODEC_STATE* codec, void* buffer, unsigned int sizebytes, unsigned int* bytesread)
{
    info* x = (info*)codec->plugindata;
    if (!x || !bytesread)
        return FMOD_ERR_INTERNAL;

    const std::uint64_t remaining = x->bufferlen - x->position;

    if (remaining == 0)
    {
        *bytesread = 0;
        return FMOD_ERR_FILE_EOF;
    }

    const unsigned int toCopy = static_cast<unsigned int>(
        std::min(static_cast<std::uint64_t>(sizebytes * static_cast<unsigned int>(x->channels) * x->format.bytesPerSample), remaining)
    );

    memset(buffer, 0, toCopy + 1);
    std::memcpy(buffer, &x->buffer[x->position], toCopy);

    x->position += toCopy;
    *bytesread   = toCopy;

    return FMOD_OK;
}

// getlengthコールバック
FMOD_RESULT F_CALL myCodec_getlength(FMOD_CODEC_STATE* codec, unsigned int* length, FMOD_TIMEUNIT lengthtype)
{
    return FMOD_OK;
}

// setpositionコールバック
FMOD_RESULT F_CALL myCodec_setposition(FMOD_CODEC_STATE* codec, int subsound, unsigned int position, FMOD_TIMEUNIT postype)
{
    info* x = (info*)codec->plugindata;
    if (!x)
        return FMOD_ERR_INTERNAL;
    x->position = position;
    return FMOD_OK;
}

// getpositionコールバック
FMOD_RESULT F_CALL myCodec_getposition(FMOD_CODEC_STATE* codec, unsigned int* position, FMOD_TIMEUNIT postype)
{
    info* x = (info*)codec->plugindata;
    if (!x)
        return FMOD_ERR_INTERNAL;
    *position = static_cast<unsigned int>(x->position);
    return FMOD_OK;
}

// soundcreatedコールバック
FMOD_RESULT F_CALL myCodec_soundcreated(FMOD_CODEC_STATE* codec, int subsound, FMOD_SOUND* sound)
{
    return FMOD_OK;
}

// getWaveFormatコールバック
FMOD_RESULT F_CALL myCodec_getWaveFormat(FMOD_CODEC_STATE* codec, int index, FMOD_CODEC_WAVEFORMAT* waveformat)
{
    info* x = (info*)codec->plugindata;
    if (!x)
        return FMOD_ERR_INTERNAL;

    waveformat->channels   = static_cast<int>(x->channels);
    waveformat->format     = x->format.fmodFormat;
    waveformat->mode       = FMOD_DEFAULT;
    waveformat->frequency  = static_cast<int>(x->sample_rates);
    waveformat->lengthpcm  = static_cast<unsigned int>(x->lengthpcm);

    return FMOD_OK;
}

// コーデックの情報
FMOD_CODEC_DESCRIPTION myCodec = {
    FMOD_CODEC_PLUGIN_VERSION,
    "FMOD MP4/AAC+ALAC Codec",
    0x00010001,
    0,
    FMOD_TIMEUNIT_PCMBYTES,
    &myCodec_open,
    &myCodec_close,
    &myCodec_read,
    &myCodec_getlength,
    &myCodec_setposition,
    &myCodec_getposition,
    &myCodec_soundcreated,
    &myCodec_getWaveFormat
};

#ifdef __cplusplus
extern "C" {
#endif
    __declspec(dllexport) FMOD_CODEC_DESCRIPTION* F_API FMODGetCodecDescription()
    {
        return &myCodec;
    }
#ifdef __cplusplus
}
#endif

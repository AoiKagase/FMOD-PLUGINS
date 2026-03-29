// main2.cpp
// FMOD APE (Monkey's Audio) コーデックプラグイン
//
// =========================================================
// 外部ライブラリ: Monkey's Audio SDK (MAC Library)
// 入手先: https://www.monkeysaudio.com/developers.html
//
// 以下のようにファイルを配置してください:
//   mac/All.h
//   mac/MACLib.h
//   mac/CharacterHelper.h          (SDK内に含まれている場合)
//   mac/lib/x64/Release/MACLib.lib
//   mac/lib/x64/Debug/MACLib.lib
//   mac/lib/Win32/Release/MACLib.lib
//   mac/lib/Win32/Debug/MACLib.lib
//
// MAC SDK バージョン: 10.x 系 (64bit int API) を対象としています。
// 古いバージョン (8.x 以前) では GetInfo/GetData の引数型が int になります。
// その場合は int64 を int に変更してください。
// =========================================================

#include <memory>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <windows.h>

#define PLATFORM_WINDOWS 1

#include "mac/All.h"
#include "mac/MACLib.h"

#include "fmod.h"

// =========================================================
// PCMフォーマット情報
// =========================================================
struct APEPCMFormatInfo
{
    FMOD_SOUND_FORMAT fmodFormat;
    unsigned int      bytesPerSample;
};

// =========================================================
// コーデック状態
// =========================================================
struct apeinfo
{
    std::uint32_t          sample_rates;
    std::uint32_t          channels;
    APEPCMFormatInfo       format;
    std::vector<std::byte> buffer;
    unsigned long          bufferlen;
    std::uint64_t          lengthpcm;   // 総PCMフレーム数 (チャンネルあたり)
    std::uint32_t          position;    // バッファ内バイトオフセット

    // メタデータ (APEv2 タグ)
    std::vector<std::byte> title;
    std::vector<std::byte> artist;
    std::vector<std::byte> album;
};

// =========================================================
// ビット深度からFMODフォーマットへのマッピング
// =========================================================
static APEPCMFormatInfo resolveAPEFormat(int bitsPerSample)
{
    if (bitsPerSample <= 16) return { FMOD_SOUND_FORMAT_PCM16, 2 };
    if (bitsPerSample <= 24) return { FMOD_SOUND_FORMAT_PCM24, 3 };
    return                         { FMOD_SOUND_FORMAT_PCM32, 4 };
}

// =========================================================
// APEv2タグ読み取り
// APEv2フッター ("APETAGEX") を末尾から探し、タグアイテムを解析する
// =========================================================
static void readAPEv2Tags(const uint8_t* fileData, uint32_t fileSize, apeinfo* x)
{
    // フッターは末尾 32 バイト
    if (fileSize < 32) return;

    // ID3v1 タグが末尾にある場合は 128 バイト手前も確認
    const uint32_t searchOffsets[] = { fileSize - 32, (fileSize > 160 ? fileSize - 160 : 0) };

    const uint8_t* footer = nullptr;
    for (uint32_t off : searchOffsets)
    {
        if (memcmp(fileData + off, "APETAGEX", 8) == 0)
        {
            footer = fileData + off;
            break;
        }
    }
    if (!footer) return;

    // フッター解析
    const uint32_t version   = footer[8]  | (footer[9]  << 8) | (footer[10] << 16) | (footer[11] << 24);
    const uint32_t tagSize   = footer[12] | (footer[13] << 8) | (footer[14] << 16) | (footer[15] << 24);
    const uint32_t itemCount = footer[16] | (footer[17] << 8) | (footer[18] << 16) | (footer[19] << 24);
    const uint32_t flags     = footer[20] | (footer[21] << 8) | (footer[22] << 16) | (footer[23] << 24);
    (void)version;

    // フッターの直前にタグアイテムが並ぶ
    const bool hasHeader = (flags >> 31) & 1;
    const uint32_t headerSize = hasHeader ? 32 : 0;
    if (tagSize < headerSize + 32) return;

    const uint32_t itemsSize = tagSize - 32; // フッター自体の 32 バイトを除く
    if ((uint32_t)(footer - fileData) < itemsSize) return;

    const uint8_t* itemsStart = footer - itemsSize;
    const uint8_t* itemsEnd   = footer;

    uint32_t parsed = 0;
    const uint8_t* p = itemsStart;
    while (p + 9 <= itemsEnd && parsed < itemCount)
    {
        const uint32_t valueLen   = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
        // p[4..7] = item flags (ignored here)
        p += 8;

        // キーは null 終端 ASCII
        const uint8_t* keyStart = p;
        while (p < itemsEnd && *p != 0) p++;
        if (p >= itemsEnd) break;
        std::string key(reinterpret_cast<const char*>(keyStart), p - keyStart);
        p++; // null 終端をスキップ

        if (p + valueLen > itemsEnd) break;
        const std::string value(reinterpret_cast<const char*>(p), valueLen);
        p += valueLen;
        parsed++;

        // キー比較 (大文字小文字を無視)
        std::string keyLower = key;
        for (auto& c : keyLower) c = (char)tolower((unsigned char)c);

        if (keyLower == "title")
            x->title.assign((std::byte*)value.data(), (std::byte*)value.data() + value.size());
        else if (keyLower == "artist")
            x->artist.assign((std::byte*)value.data(), (std::byte*)value.data() + value.size());
        else if (keyLower == "album")
            x->album.assign((std::byte*)value.data(), (std::byte*)value.data() + value.size());
    }
}

// =========================================================
// openコールバック
// =========================================================
FMOD_RESULT F_CALL apeCodec_open(FMOD_CODEC_STATE* codec, FMOD_MODE usermode, FMOD_CREATESOUNDEXINFO* userexinfo)
{
    if (!codec) return FMOD_ERR_INTERNAL;

    unsigned int totalSize = 0;
    codec->functions->size(codec, &totalSize);
    if (totalSize < 8) return FMOD_ERR_FILE_EOF;

    // APEマジック確認: "MAC "
    uint8_t magic[4] = {};
    unsigned int rb = 0;
    codec->functions->read(codec, magic, 4, &rb);
    if (rb < 4 || memcmp(magic, "MAC ", 4) != 0)
        return FMOD_ERR_FORMAT;

    // ファイル全体をメモリに読み込む
    codec->functions->seek(codec, 0, FMOD_CODEC_SEEK_METHOD_SET);
    std::vector<uint8_t> fileData(totalSize);
    codec->functions->read(codec, fileData.data(), totalSize, &rb);
    if (rb < totalSize) return FMOD_ERR_FILE_BAD;

    // =========================================================
    // 一時ファイルに書き出す
    // MAC SDK は IAPEDecompress をファイルパスから開くため、
    // FMOD のメモリコールバックを直接渡せない。
    // 一時 .ape ファイルを作成し、デコード後に削除する。
    // =========================================================
    wchar_t tempDir[MAX_PATH]  = {};
    wchar_t tempBase[MAX_PATH] = {};
    wchar_t tempApe[MAX_PATH]  = {};

    if (GetTempPathW(MAX_PATH, tempDir) == 0)
        return FMOD_ERR_INTERNAL;

    // GetTempFileNameW が作る空ファイル (後で削除)
    if (GetTempFileNameW(tempDir, L"fma", 0, tempBase) == 0)
        return FMOD_ERR_INTERNAL;

    // .ape 拡張子付きの実際の一時ファイル名
    wcsncpy_s(tempApe, tempBase, MAX_PATH - 5);
    wcsncat_s(tempApe, L".ape", 5);

    HANDLE hFile = CreateFileW(tempApe, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        DeleteFileW(tempBase);
        return FMOD_ERR_INTERNAL;
    }

    DWORD written = 0;
    const BOOL writeOK = WriteFile(hFile, fileData.data(), totalSize, &written, nullptr);
    CloseHandle(hFile);
    DeleteFileW(tempBase); // GetTempFileNameW が作った空ファイルを削除

    if (!writeOK || written < totalSize)
    {
        DeleteFileW(tempApe);
        return FMOD_ERR_INTERNAL;
    }

    // =========================================================
    // MAC SDK で APE ファイルを開く
    // =========================================================
    int nRetVal = ERROR_SUCCESS;
    APE::IAPEDecompress* pDecompress = CreateIAPEDecompress(tempApe, &nRetVal, TRUE, TRUE, FALSE);
    if (!pDecompress || nRetVal != ERROR_SUCCESS)
    {
        DeleteFileW(tempApe);
        return FMOD_ERR_FORMAT;
    }

    const int64_t nChannels      = pDecompress->GetInfo(APE::IAPEDecompress::APE_DECOMPRESS_FIELDS::APE_INFO_CHANNELS);
    const int64_t nSampleRate    = pDecompress->GetInfo(APE::IAPEDecompress::APE_DECOMPRESS_FIELDS::APE_INFO_SAMPLE_RATE);
    const int64_t nBitsPerSample = pDecompress->GetInfo(APE::IAPEDecompress::APE_DECOMPRESS_FIELDS::APE_INFO_BITS_PER_SAMPLE);
    const int64_t nTotalBlocks   = pDecompress->GetInfo(APE::IAPEDecompress::APE_DECOMPRESS_FIELDS::APE_DECOMPRESS_TOTAL_BLOCKS);

    if (nChannels <= 0 || nSampleRate <= 0 || nBitsPerSample <= 0 || nTotalBlocks <= 0)
    {
        delete pDecompress;
        DeleteFileW(tempApe);
        return FMOD_ERR_FORMAT;
    }

    APEPCMFormatInfo fmt = resolveAPEFormat(static_cast<int>(nBitsPerSample));

    // PCM バッファを確保
    const int64_t bytesPerBlock  = nChannels * static_cast<__int64>(fmt.bytesPerSample);
    const int64_t totalPCMBytes  = nTotalBlocks * bytesPerBlock;

    std::vector<uint8_t> pcmBuffer(static_cast<size_t>(totalPCMBytes));

    // =========================================================
    // ブロック単位でデコード (全フレームを一括バッファに展開)
    // =========================================================
    const int64_t DECODE_CHUNK = 65536; // 一度に処理するブロック数
    int64_t decoded = 0;

    while (decoded < nTotalBlocks)
    {
        const int64_t toRead  = min(DECODE_CHUNK, nTotalBlocks - decoded);
        int64_t       gotBlocks = 0;
        const int   ret = pDecompress->GetData(
            reinterpret_cast<unsigned char*>(pcmBuffer.data()) + decoded * bytesPerBlock,
            toRead, &gotBlocks);

        if (ret != ERROR_SUCCESS || gotBlocks <= 0)
            break;
        decoded += gotBlocks;
    }

    delete pDecompress;
    DeleteFileW(tempApe);

    if (decoded == 0)
        return FMOD_ERR_FORMAT;

    // =========================================================
    // コーデック状態に格納
    // =========================================================
    auto x = std::make_unique<apeinfo>();
    x->sample_rates = static_cast<std::uint32_t>(nSampleRate);
    x->channels     = static_cast<std::uint32_t>(nChannels);
    x->format       = fmt;
    x->bufferlen    = static_cast<unsigned long>(decoded * bytesPerBlock);
    x->buffer.assign(reinterpret_cast<std::byte*>(pcmBuffer.data()),
                     reinterpret_cast<std::byte*>(pcmBuffer.data()) + x->bufferlen);
    x->lengthpcm    = static_cast<std::uint64_t>(decoded);
    x->position     = 0;

    // APEv2 タグ読み取り
    readAPEv2Tags(fileData.data(), totalSize, x.get());

    if (!x->title.empty())
        codec->functions->metadata(codec, FMOD_TAGTYPE_USER, (char*)"TITLE",
            x->title.data(), static_cast<unsigned int>(x->title.size()),
            FMOD_TAGDATATYPE_STRING_UTF8, 1);
    if (!x->artist.empty())
        codec->functions->metadata(codec, FMOD_TAGTYPE_USER, (char*)"ARTIST",
            x->artist.data(), static_cast<unsigned int>(x->artist.size()),
            FMOD_TAGDATATYPE_STRING_UTF8, 1);
    if (!x->album.empty())
        codec->functions->metadata(codec, FMOD_TAGTYPE_USER, (char*)"ALBUM",
            x->album.data(), static_cast<unsigned int>(x->album.size()),
            FMOD_TAGDATATYPE_STRING_UTF8, 1);

    codec->numsubsounds = 0;
    codec->plugindata   = x.release();
    return FMOD_OK;
}

// =========================================================
// closeコールバック
// =========================================================
FMOD_RESULT F_CALL apeCodec_close(FMOD_CODEC_STATE* codec)
{
    if (codec->plugindata)
    {
        delete static_cast<apeinfo*>(codec->plugindata);
        codec->plugindata = nullptr;
    }
    return FMOD_OK;
}

// =========================================================
// readコールバック
// sizebytes: FMODが要求するPCMフレーム数
// 戻り値: 実際に書き込んだバイト数 (*bytesread)
// =========================================================
FMOD_RESULT F_CALL apeCodec_read(FMOD_CODEC_STATE* codec, void* buffer,
                                  unsigned int sizebytes, unsigned int* bytesread)
{
    apeinfo* x = static_cast<apeinfo*>(codec->plugindata);
    if (!x || !bytesread) return FMOD_ERR_INTERNAL;

    const std::uint64_t remaining = x->bufferlen - x->position;
    if (remaining == 0)
    {
        *bytesread = 0;
        return FMOD_ERR_FILE_EOF;
    }

    const unsigned int toCopy = static_cast<unsigned int>(
        min(static_cast<std::uint64_t>(
            sizebytes * x->channels * x->format.bytesPerSample), remaining)
    );

    memset(buffer, 0, toCopy + 1);
    std::memcpy(buffer, &x->buffer[x->position], toCopy);
    x->position += toCopy;
    *bytesread   = toCopy;
    return FMOD_OK;
}

// =========================================================
// getlengthコールバック
// =========================================================
FMOD_RESULT F_CALL apeCodec_getlength(FMOD_CODEC_STATE* codec, unsigned int* length,
                                       FMOD_TIMEUNIT lengthtype)
{
    return FMOD_OK;
}

// =========================================================
// setpositionコールバック
// =========================================================
FMOD_RESULT F_CALL apeCodec_setposition(FMOD_CODEC_STATE* codec, int subsound,
                                         unsigned int position, FMOD_TIMEUNIT postype)
{
    apeinfo* x = static_cast<apeinfo*>(codec->plugindata);
    if (!x) return FMOD_ERR_INTERNAL;
    x->position = position;
    return FMOD_OK;
}

// =========================================================
// getpositionコールバック
// =========================================================
FMOD_RESULT F_CALL apeCodec_getposition(FMOD_CODEC_STATE* codec, unsigned int* position,
                                         FMOD_TIMEUNIT postype)
{
    apeinfo* x = static_cast<apeinfo*>(codec->plugindata);
    if (!x) return FMOD_ERR_INTERNAL;
    *position = x->position;
    return FMOD_OK;
}

// =========================================================
// soundcreatedコールバック
// =========================================================
FMOD_RESULT F_CALL apeCodec_soundcreated(FMOD_CODEC_STATE* codec, int subsound, FMOD_SOUND* sound)
{
    return FMOD_OK;
}

// =========================================================
// getWaveFormatコールバック
// =========================================================
FMOD_RESULT F_CALL apeCodec_getWaveFormat(FMOD_CODEC_STATE* codec, int index,
                                           FMOD_CODEC_WAVEFORMAT* waveformat)
{
    apeinfo* x = static_cast<apeinfo*>(codec->plugindata);
    if (!x) return FMOD_ERR_INTERNAL;

    waveformat->channels  = static_cast<int>(x->channels);
    waveformat->format    = x->format.fmodFormat;
    waveformat->mode      = FMOD_DEFAULT;
    waveformat->frequency = static_cast<int>(x->sample_rates);
    waveformat->lengthpcm = static_cast<unsigned int>(x->lengthpcm);
    return FMOD_OK;
}

// =========================================================
// コーデック記述子
// =========================================================
FMOD_CODEC_DESCRIPTION apeCodecDesc = {
    FMOD_CODEC_PLUGIN_VERSION,
    "FMOD APE (Monkey's Audio) Codec",
    0x00010000,
    0,
    FMOD_TIMEUNIT_PCMBYTES,
    &apeCodec_open,
    &apeCodec_close,
    &apeCodec_read,
    &apeCodec_getlength,
    &apeCodec_setposition,
    &apeCodec_getposition,
    &apeCodec_soundcreated,
    &apeCodec_getWaveFormat
};

#ifdef __cplusplus
extern "C" {
#endif
    __declspec(dllexport) FMOD_CODEC_DESCRIPTION* F_API FMODGetCodecDescription()
    {
        return &apeCodecDesc;
    }
#ifdef __cplusplus
}
#endif

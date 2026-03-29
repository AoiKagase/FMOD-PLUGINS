// main.cpp
// FMOD TAK (Tom's lossless Audio Kompressor) コーデックプラグイン
//
// =========================================================
// 外部ライブラリ: TAK SDK (tak_deco_lib) バージョン 2.3.3
// 入手先: http://www.thbeck.de/Tak/Tak.html
//
// 必要なファイル:
//   tak/tak_deco_lib.h    ← ヘッダのみ (コンパイル時)
//   tak_deco_lib.dll      ← 実行時に codec_tak.dll と同フォルダに配置
//
// .lib ファイルは不要です。
// DLL を LoadLibrary で実行時動的ロードしています。
// =========================================================

#include <memory>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// ヘッダは型定義のためだけに使用する。
// TAK_API = __declspec(dllimport) の関数宣言は参照しない。
#include "tak/tak_deco_lib.h"

#include "../fmod.h"

// =========================================================
// TAK SDK 関数ポインタ (LoadLibrary 経由で解決)
// =========================================================
static HMODULE g_hTak = nullptr;

static TtakSeekableStreamDecoder (*g_SSD_Create_FromStream)(
    const TtakStreamIoInterface*, void*,
    const TtakSSDOptions*, TSSDDamageCallback, void*) = nullptr;

static void     (*g_SSD_Destroy)       (TtakSeekableStreamDecoder)                            = nullptr;
static TtakBool (*g_SSD_Valid)         (TtakSeekableStreamDecoder)                            = nullptr;
static TtakResult (*g_SSD_State)       (TtakSeekableStreamDecoder)                            = nullptr;
static TtakResult (*g_SSD_GetStreamInfo)(TtakSeekableStreamDecoder, Ttak_str_StreamInfo*)     = nullptr;
static TtakResult (*g_SSD_ReadAudio)   (TtakSeekableStreamDecoder, void*, TtakInt32, TtakInt32*) = nullptr;
static TtakAPEv2Tag (*g_SSD_GetAPEv2Tag)(TtakSeekableStreamDecoder)                          = nullptr;
static TtakBool (*g_APE_Valid)         (TtakAPEv2Tag)                                         = nullptr;
static TtakResult (*g_APE_GetIndexOfKey)(TtakAPEv2Tag, const TtakAnsiChar*, TtakInt32*)       = nullptr;
static TtakResult (*g_APE_GetTextItemValueAsAnsi)(
    TtakAPEv2Tag, TtakInt32, TtakInt32,
    TtakAnsiChar, TtakAnsiChar*, TtakInt32, TtakInt32*) = nullptr;

// =========================================================
// DLL ロード (初回 open 時に一度だけ実行)
// =========================================================
static std::once_flag g_takLoadOnce;
static bool           g_takLoadOk = false;

static void tryLoadTAK()
{
    g_hTak = LoadLibraryW(L"tak_deco_lib.dll");
    if (!g_hTak) return;

    // GetProcAddress でシンボルを解決するマクロ
#define GETFN(var, name) \
    (var) = reinterpret_cast<decltype(var)>(GetProcAddress(g_hTak, (name))); \
    if (!(var)) { FreeLibrary(g_hTak); g_hTak = nullptr; return; }

    GETFN(g_SSD_Create_FromStream,      "tak_SSD_Create_FromStream")
    GETFN(g_SSD_Destroy,                "tak_SSD_Destroy")
    GETFN(g_SSD_Valid,                  "tak_SSD_Valid")
    GETFN(g_SSD_State,                  "tak_SSD_State")
    GETFN(g_SSD_GetStreamInfo,          "tak_SSD_GetStreamInfo")
    GETFN(g_SSD_ReadAudio,              "tak_SSD_ReadAudio")
    GETFN(g_SSD_GetAPEv2Tag,            "tak_SSD_GetAPEv2Tag")
    GETFN(g_APE_Valid,                  "tak_APE_Valid")
    GETFN(g_APE_GetIndexOfKey,          "tak_APE_GetIndexOfKey")
    GETFN(g_APE_GetTextItemValueAsAnsi, "tak_APE_GetTextItemValueAsAnsi")
#undef GETFN

    g_takLoadOk = true;
}

// =========================================================
// PCMフォーマット情報
// =========================================================
struct TAKPCMFormat
{
    FMOD_SOUND_FORMAT fmodFormat;
    unsigned int      bytesPerSample;
};

// =========================================================
// コーデック状態
// =========================================================
struct takinfo
{
    uint32_t               sample_rates;
    uint32_t               channels;
    TAKPCMFormat           format;
    std::vector<std::byte> buffer;
    unsigned long          bufferlen;
    uint64_t               lengthpcm;
    uint32_t               position;

    std::vector<std::byte> title;
    std::vector<std::byte> artist;
    std::vector<std::byte> album;
};

static TAKPCMFormat resolveTAKFormat(int sampleBits)
{
    if (sampleBits <= 16) return { FMOD_SOUND_FORMAT_PCM16, 2 };
    if (sampleBits <= 24) return { FMOD_SOUND_FORMAT_PCM24, 3 };
    return                      { FMOD_SOUND_FORMAT_PCM32, 4 };
}

// =========================================================
// FMOD コールバック → TtakStreamIoInterface ブリッジ
// =========================================================
struct FMODTakIO
{
    FMOD_CODEC_STATE* codec;
    uint64_t          position;
    uint64_t          fileSize;
};

static TtakBool tak_CanRead (void*)  { return tak_True;  }
static TtakBool tak_CanWrite(void*)  { return tak_False; }
static TtakBool tak_CanSeek (void*)  { return tak_True;  }

static TtakBool tak_Read(void* user, void* buf, TtakInt32 num, TtakInt32* readNum)
{
    FMODTakIO*   io = static_cast<FMODTakIO*>(user);
    unsigned int rb = 0;
    io->codec->functions->read(io->codec, buf, static_cast<unsigned int>(num), &rb);
    io->position += rb;
    *readNum = static_cast<TtakInt32>(rb);
    return tak_True;
}

static TtakBool tak_Write   (void*, const void*, TtakInt32) { return tak_False; }
static TtakBool tak_Flush   (void*)                         { return tak_True;  }
static TtakBool tak_Truncate(void*)                         { return tak_False; }

static TtakBool tak_Seek(void* user, TtakInt64 pos)
{
    FMODTakIO* io = static_cast<FMODTakIO*>(user);
    io->codec->functions->seek(io->codec,
        static_cast<unsigned int>(pos), FMOD_CODEC_SEEK_METHOD_SET);
    io->position = static_cast<uint64_t>(pos);
    return tak_True;
}

static TtakBool tak_GetLength(void* user, TtakInt64* length)
{
    FMODTakIO* io = static_cast<FMODTakIO*>(user);
    *length = static_cast<TtakInt64>(io->fileSize);
    return tak_True;
}

// =========================================================
// openコールバック
// =========================================================
FMOD_RESULT F_CALL takCodec_open(FMOD_CODEC_STATE* codec, FMOD_MODE usermode,
                                   FMOD_CREATESOUNDEXINFO* userexinfo)
{
    if (!codec) return FMOD_ERR_INTERNAL;

    // 初回のみ DLL をロード
    std::call_once(g_takLoadOnce, tryLoadTAK);
    if (!g_takLoadOk) return FMOD_ERR_INITIALIZATION; // DLL が見つからない

    unsigned int totalSize = 0;
    codec->functions->size(codec, &totalSize);
    if (totalSize < 8) return FMOD_ERR_FILE_EOF;

    // =========================================================
    // IO インターフェース設定
    // =========================================================
    FMODTakIO ioCtx = {};
    ioCtx.codec     = codec;
    ioCtx.position  = 0;
    ioCtx.fileSize  = totalSize;

    TtakStreamIoInterface iface = {};
    iface.CanRead   = tak_CanRead;
    iface.CanWrite  = tak_CanWrite;
    iface.CanSeek   = tak_CanSeek;
    iface.Read      = tak_Read;
    iface.Write     = tak_Write;
    iface.Flush     = tak_Flush;
    iface.Truncate  = tak_Truncate;
    iface.Seek      = tak_Seek;
    iface.GetLength = tak_GetLength;

    TtakSSDOptions options = {};
    options.Cpu   = tak_Cpu_Any;
    options.Flags = 0;

    // =========================================================
    // デコーダ生成 (失敗 = 非TAKファイル)
    // =========================================================
    TtakSeekableStreamDecoder decoder =
        g_SSD_Create_FromStream(&iface, &ioCtx, &options, nullptr, nullptr);

    if (!decoder || !g_SSD_Valid(decoder))
        return FMOD_ERR_FORMAT;

    if (g_SSD_State(decoder) >= tak_res_ssd_ErrorFirst)
    {
        g_SSD_Destroy(decoder);
        return FMOD_ERR_FORMAT;
    }

    // =========================================================
    // ストリーム情報取得
    // =========================================================
    Ttak_str_StreamInfo si = {};
    if (g_SSD_GetStreamInfo(decoder, &si) != tak_res_Ok)
    {
        g_SSD_Destroy(decoder);
        return FMOD_ERR_FORMAT;
    }

    const int     nChannels   = si.Audio.ChannelNum;
    const int     nSampleRate = si.Audio.SampleRate;
    const int     nSampleBits = si.Audio.SampleBits;
    const int64_t nFrames     = si.Sizes.SampleNum;

    if (nChannels <= 0 || nSampleRate <= 0 || nSampleBits <= 0 || nFrames <= 0)
    {
        g_SSD_Destroy(decoder);
        return FMOD_ERR_FORMAT;
    }

    // BlockSize = 1フレーム分の総バイト数 (全チャンネル)
    const TAKPCMFormat fmt = resolveTAKFormat(nSampleBits);
    const unsigned int bytesPerSample =
        (si.Audio.BlockSize > 0 && nChannels > 0)
            ? static_cast<unsigned int>(si.Audio.BlockSize / nChannels)
            : fmt.bytesPerSample;

    std::vector<uint8_t> pcmBuffer(
        static_cast<size_t>(nFrames) * nChannels * bytesPerSample);

    // =========================================================
    // 全フレームをチャンク単位でデコード
    // =========================================================
    const TtakInt32 CHUNK   = 65536;
    int64_t         decoded = 0;

    while (decoded < nFrames)
    {
        const TtakInt32 toRead =
            static_cast<TtakInt32>(std::min(static_cast<int64_t>(CHUNK), nFrames - decoded));
        TtakInt32  got = 0;
        TtakResult r   = g_SSD_ReadAudio(
            decoder,
            pcmBuffer.data() + decoded * nChannels * bytesPerSample,
            toRead, &got);

        if (r >= tak_res_ssd_ErrorFirst || got <= 0) break;
        decoded += got;
    }

    // =========================================================
    // APEv2 タグ読み取り (SDK ネイティブ API)
    // =========================================================
    auto x = std::make_unique<takinfo>();

    TtakAPEv2Tag tag = g_SSD_GetAPEv2Tag(decoder);
    if (g_APE_Valid(tag))
    {
        auto readTag = [&](const TtakAnsiChar* key, std::vector<std::byte>& dest)
        {
            TtakInt32 idx  = 0;
            TtakInt32 size = 0;
            if (g_APE_GetIndexOfKey(tag, key, &idx) != tak_res_Ok) return;

            char buf[1024] = {};
            if (g_APE_GetTextItemValueAsAnsi(
                    tag, idx, 0, ',', buf,
                    static_cast<TtakInt32>(sizeof(buf) - 1), &size) == tak_res_Ok)
            {
                dest.assign(reinterpret_cast<std::byte*>(buf),
                            reinterpret_cast<std::byte*>(buf) + size);
            }
        };

        readTag("Title",  x->title);
        readTag("Artist", x->artist);
        readTag("Album",  x->album);
    }

    g_SSD_Destroy(decoder);

    if (decoded == 0) return FMOD_ERR_FORMAT;

    if (!x->title.empty())
        codec->functions->metadata(codec, FMOD_TAGTYPE_USER, (char*)"TITLE",
            x->title.data(), (unsigned int)x->title.size(), FMOD_TAGDATATYPE_STRING_UTF8, 1);
    if (!x->artist.empty())
        codec->functions->metadata(codec, FMOD_TAGTYPE_USER, (char*)"ARTIST",
            x->artist.data(), (unsigned int)x->artist.size(), FMOD_TAGDATATYPE_STRING_UTF8, 1);
    if (!x->album.empty())
        codec->functions->metadata(codec, FMOD_TAGTYPE_USER, (char*)"ALBUM",
            x->album.data(), (unsigned int)x->album.size(), FMOD_TAGDATATYPE_STRING_UTF8, 1);

    const unsigned long actualBytes =
        static_cast<unsigned long>(decoded) * nChannels * bytesPerSample;

    x->sample_rates = static_cast<uint32_t>(nSampleRate);
    x->channels     = static_cast<uint32_t>(nChannels);
    x->format       = { fmt.fmodFormat, bytesPerSample };
    x->bufferlen    = actualBytes;
    x->buffer.assign(reinterpret_cast<std::byte*>(pcmBuffer.data()),
                     reinterpret_cast<std::byte*>(pcmBuffer.data()) + actualBytes);
    x->lengthpcm    = static_cast<uint64_t>(decoded);
    x->position     = 0;

    codec->numsubsounds = 0;
    codec->plugindata   = x.release();
    return FMOD_OK;
}

// =========================================================
// closeコールバック
// =========================================================
FMOD_RESULT F_CALL takCodec_close(FMOD_CODEC_STATE* codec)
{
    if (codec->plugindata)
    {
        delete static_cast<takinfo*>(codec->plugindata);
        codec->plugindata = nullptr;
    }
    return FMOD_OK;
}

// =========================================================
// readコールバック
// =========================================================
FMOD_RESULT F_CALL takCodec_read(FMOD_CODEC_STATE* codec, void* buffer,
                                   unsigned int sizebytes, unsigned int* bytesread)
{
    takinfo* x = static_cast<takinfo*>(codec->plugindata);
    if (!x || !bytesread) return FMOD_ERR_INTERNAL;

    const uint64_t remaining = x->bufferlen - x->position;
    if (remaining == 0)
    {
        *bytesread = 0;
        return FMOD_ERR_FILE_EOF;
    }

    const unsigned int toCopy = static_cast<unsigned int>(
        std::min(static_cast<uint64_t>(sizebytes * x->channels * x->format.bytesPerSample),
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
FMOD_RESULT F_CALL takCodec_getlength(FMOD_CODEC_STATE*, unsigned int*, FMOD_TIMEUNIT)
{
    return FMOD_OK;
}

FMOD_RESULT F_CALL takCodec_setposition(FMOD_CODEC_STATE* codec, int,
                                          unsigned int position, FMOD_TIMEUNIT)
{
    takinfo* x = static_cast<takinfo*>(codec->plugindata);
    if (!x) return FMOD_ERR_INTERNAL;
    x->position = position;
    return FMOD_OK;
}

FMOD_RESULT F_CALL takCodec_getposition(FMOD_CODEC_STATE* codec,
                                          unsigned int* position, FMOD_TIMEUNIT)
{
    takinfo* x = static_cast<takinfo*>(codec->plugindata);
    if (!x) return FMOD_ERR_INTERNAL;
    *position = x->position;
    return FMOD_OK;
}

FMOD_RESULT F_CALL takCodec_soundcreated(FMOD_CODEC_STATE*, int, FMOD_SOUND*)
{
    return FMOD_OK;
}

FMOD_RESULT F_CALL takCodec_getWaveFormat(FMOD_CODEC_STATE* codec, int,
                                            FMOD_CODEC_WAVEFORMAT* wf)
{
    takinfo* x = static_cast<takinfo*>(codec->plugindata);
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
FMOD_CODEC_DESCRIPTION takCodecDesc = {
    FMOD_CODEC_PLUGIN_VERSION,
    "FMOD TAK (Tom's lossless Audio Kompressor) Codec",
    0x00010000,
    0,
    FMOD_TIMEUNIT_PCMBYTES,
    &takCodec_open,
    &takCodec_close,
    &takCodec_read,
    &takCodec_getlength,
    &takCodec_setposition,
    &takCodec_getposition,
    &takCodec_soundcreated,
    &takCodec_getWaveFormat
};

#ifdef __cplusplus
extern "C" {
#endif
    __declspec(dllexport) FMOD_CODEC_DESCRIPTION* F_API FMODGetCodecDescription()
    {
        return &takCodecDesc;
    }
#ifdef __cplusplus
}
#endif

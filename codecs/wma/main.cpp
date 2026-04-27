// main.cpp
// FMOD WMA コーデックプラグイン
//
// =========================================================
// Windows Media Foundation (IMFSourceReader) を使用して
// WMA ファイルを PCM にデコードする FMOD コーデックプラグイン。
//
// 外部ライブラリ不要 - Windows SDK 組み込みのみ使用:
//   mfplat.lib, mfreadwrite.lib, mfuuid.lib, Shlwapi.lib, Propsys.lib
//
// 対応フォーマット:
//   WMA Standard / WMA Pro / WMA Lossless
//
// ビルド方法:
//   codec_wma.sln を Visual Studio 2022 で開いてビルド
// =========================================================

#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>
#include <mutex>
#include <initializer_list>
#include <cctype>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Propsys.lib")

#include "fmod.h"
#include "fmod_codec.h"

// =========================================================
// MFStartup / MFShutdown 参照カウント管理
// =========================================================
static std::once_flag g_mfInitOnce;
static bool           g_mfInitOk    = false;
static std::mutex     g_mfRefMutex;
static int            g_mfRefCount  = 0;

static bool mfAddRef()
{
    std::call_once(g_mfInitOnce, []()
    {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        g_mfInitOk = SUCCEEDED(hr);
    });
    if (!g_mfInitOk) return false;
    std::lock_guard<std::mutex> lock(g_mfRefMutex);
    ++g_mfRefCount;
    return true;
}

static void mfRelease()
{
    std::lock_guard<std::mutex> lock(g_mfRefMutex);
    --g_mfRefCount;
}

// =========================================================
// ASF ヘッダ GUID の先頭 4 バイト (WMA/WMV コンテナ識別)
// =========================================================
static const uint8_t k_ASFMagic[4] = { 0x30, 0x26, 0xB2, 0x75 };

// =========================================================
// PCM フォーマット情報
// =========================================================
struct PCMFormatInfo
{
    FMOD_SOUND_FORMAT fmodFormat;
    GUID              mfSubtype;
    int               bytesPerSample;
};

// ネイティブ bps に応じて出力フォーマットを決定する。
// MFAudioFormat_PCM は 16bit まで確実にサポート。
// 17bit 以上は MFAudioFormat_Float (32bit) に昇格させる。
static PCMFormatInfo resolveWmaFormat(UINT32 nativeBps)
{
    if (nativeBps > 16)
        return { FMOD_SOUND_FORMAT_PCMFLOAT, MFAudioFormat_Float, 4 };
    return { FMOD_SOUND_FORMAT_PCM16, MFAudioFormat_PCM, 2 };
}

// =========================================================
// コーデック状態 (codec_wv の WvInfo と同一パターン)
// =========================================================
struct WmaInfo
{
    std::vector<uint8_t>  buffer;
    std::string           title;
    std::string           artist;
    std::string           album;
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

// =========================================================
// ヘルパー: wstring → UTF-8 std::string
// =========================================================
static std::string wstrToUtf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0,
        ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
        result.data(), len, nullptr, nullptr);
    return result;
}

static void splitTrackValue(const std::string& value, std::string& number, std::string& total)
{
    const auto trim = [](std::string s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
            s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.pop_back();
        return s;
    };

    number.clear();
    total.clear();

    const size_t slash = value.find('/');
    if (slash == std::string::npos)
    {
        number = trim(value);
        return;
    }

    number = trim(value.substr(0, slash));
    total = trim(value.substr(slash + 1));
}

// =========================================================
// open コールバック
// =========================================================
static FMOD_RESULT F_CALL wmaCodec_open(FMOD_CODEC_STATE* codec,
    FMOD_MODE /*usermode*/, FMOD_CREATESOUNDEXINFO* /*userexinfo*/)
{
    if (!codec) return FMOD_ERR_INTERNAL;

    // MFStartup
    if (!mfAddRef()) return FMOD_ERR_INITIALIZATION;

    // ファイルサイズ取得
    unsigned int totalSize = 0;
    codec->functions->size(codec, &totalSize);
    if (totalSize < 4)
    {
        mfRelease();
        return FMOD_ERR_FILE_EOF;
    }

    // ASF マジックバイト確認
    uint8_t      magic[4] = {};
    unsigned int rb       = 0;
    codec->functions->read(codec, magic, 4, &rb);
    if (rb < 4 || memcmp(magic, k_ASFMagic, 4) != 0)
    {
        mfRelease();
        return FMOD_ERR_FORMAT;
    }
    codec->functions->seek(codec, 0, FMOD_CODEC_SEEK_METHOD_SET);

    // ファイル全体をメモリに読み込む
    std::vector<uint8_t> fileData(totalSize);
    {
        unsigned int totalRead = 0;
        while (totalRead < totalSize)
        {
            unsigned int got = 0;
            codec->functions->read(codec,
                fileData.data() + totalRead,
                totalSize - totalRead, &got);
            if (got == 0) break;
            totalRead += got;
        }
    }

    // IStream 作成 (SHCreateMemStream はデータをコピーして所有する)
    IStream* pRawStream = SHCreateMemStream(fileData.data(),
        static_cast<UINT>(fileData.size()));
    if (!pRawStream)
    {
        mfRelease();
        return FMOD_ERR_MEMORY;
    }
    Microsoft::WRL::ComPtr<IStream> spStream;
    spStream.Attach(pRawStream); // AddRef 済みのポインタを Attach

    // IMFByteStream 作成
    Microsoft::WRL::ComPtr<IMFByteStream> spByteStream;
    HRESULT hr = MFCreateMFByteStreamOnStream(spStream.Get(), &spByteStream);
    if (FAILED(hr))
    {
        mfRelease();
        return FMOD_ERR_FORMAT;
    }

    // IMFSourceReader 作成
    Microsoft::WRL::ComPtr<IMFSourceReader> spReader;
    hr = MFCreateSourceReaderFromByteStream(spByteStream.Get(), nullptr, &spReader);
    if (FAILED(hr))
    {
        mfRelease();
        return FMOD_ERR_FORMAT;
    }

    // ネイティブフォーマットを取得して bps を確認
    Microsoft::WRL::ComPtr<IMFMediaType> spNativeType;
    hr = spReader->GetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, &spNativeType);
    if (FAILED(hr))
    {
        mfRelease();
        return FMOD_ERR_FORMAT;
    }
    UINT32 nativeBps = 0;
    spNativeType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &nativeBps);

    // 出力フォーマット決定
    const PCMFormatInfo fmt = resolveWmaFormat(nativeBps);

    // 出力フォーマットを PCM に設定
    Microsoft::WRL::ComPtr<IMFMediaType> spPCMType;
    hr = MFCreateMediaType(&spPCMType);
    if (FAILED(hr))
    {
        mfRelease();
        return FMOD_ERR_MEMORY;
    }
    spPCMType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    spPCMType->SetGUID(MF_MT_SUBTYPE,    fmt.mfSubtype);

    hr = spReader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, spPCMType.Get());
    if (FAILED(hr))
    {
        mfRelease();
        return FMOD_ERR_FORMAT;
    }

    // 実際の出力パラメータ取得
    Microsoft::WRL::ComPtr<IMFMediaType> spActualType;
    hr = spReader->GetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, &spActualType);
    if (FAILED(hr))
    {
        mfRelease();
        return FMOD_ERR_FORMAT;
    }

    UINT32 nChannels = 0, nSampleRate = 0;
    spActualType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,       &nChannels);
    spActualType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &nSampleRate);

    if (nChannels == 0 || nSampleRate == 0)
    {
        mfRelease();
        return FMOD_ERR_FORMAT;
    }

    // ストリームを選択
    spReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    spReader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    // =========================================================
    // ReadSample ループで全サンプルをデコード
    // =========================================================
    std::vector<uint8_t> pcmBuffer;
    pcmBuffer.reserve(
        static_cast<size_t>(nSampleRate) * nChannels * fmt.bytesPerSample * 10);

    while (true)
    {
        DWORD    streamIndex = 0, flags = 0;
        LONGLONG timeStamp   = 0;
        Microsoft::WRL::ComPtr<IMFSample> spSample;

        hr = spReader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            0, &streamIndex, &flags, &timeStamp, &spSample);

        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!spSample) continue;

        Microsoft::WRL::ComPtr<IMFMediaBuffer> spBuffer;
        if (FAILED(spSample->ConvertToContiguousBuffer(&spBuffer))) break;

        BYTE*  pData   = nullptr;
        DWORD  cbValid = 0;
        if (FAILED(spBuffer->Lock(&pData, nullptr, &cbValid))) break;

        const size_t prevSize = pcmBuffer.size();
        pcmBuffer.resize(prevSize + cbValid);
        memcpy(pcmBuffer.data() + prevSize, pData, cbValid);

        spBuffer->Unlock();
    }

    if (pcmBuffer.empty())
    {
        mfRelease();
        return FMOD_ERR_FORMAT;
    }

    // =========================================================
    // メタデータ抽出 (IMFMetadata 経由)
    // =========================================================
    std::string tagTitle, tagArtist, tagAlbum, tagYear, tagTrackNumber, tagTrackTotal;
    {
        Microsoft::WRL::ComPtr<IMFMetadataProvider> spMetaProvider;
        HRESULT hrMeta = MFGetService(spReader.Get(),
            MF_METADATA_PROVIDER_SERVICE,
            IID_PPV_ARGS(&spMetaProvider));
        if (SUCCEEDED(hrMeta))
        {
            Microsoft::WRL::ComPtr<IMFMetadata> spMeta;
            hrMeta = spMetaProvider->GetMFMetadata(nullptr, 0, 0, &spMeta);
            if (SUCCEEDED(hrMeta))
            {
                auto getTag = [&](std::initializer_list<LPCWSTR> propNames) -> std::string
                {
                    for (auto propName : propNames)
                    {
                        PROPVARIANT pv;
                        PropVariantInit(&pv);
                        std::string result;
                        if (SUCCEEDED(spMeta->GetProperty(propName, &pv)))
                        {
                            LPWSTR str = nullptr;
                            if (SUCCEEDED(PropVariantToStringAlloc(pv, &str)))
                            {
                                result = wstrToUtf8(str);
                                CoTaskMemFree(str);
                            }
                        }
                        PropVariantClear(&pv);
                        if (!result.empty())
                            return result;
                    }
                    return {};
                };

                tagTitle  = getTag({ L"Title" });
                tagArtist = getTag({ L"Author" });
                tagAlbum  = getTag({ L"WM/AlbumTitle" });
                tagYear   = getTag({ L"WM/Year", L"Year" });

                const std::string trackRaw = getTag({ L"WM/TrackNumber", L"WM/Track", L"TrackNumber", L"Track" });
                splitTrackValue(trackRaw, tagTrackNumber, tagTrackTotal);

            }
        }
    }

    // =========================================================
    // PCM フレーム数を計算
    // =========================================================
    const unsigned int totalPcmFrames = static_cast<unsigned int>(
        pcmBuffer.size() / (static_cast<size_t>(nChannels) * fmt.bytesPerSample));

    // =========================================================
    // コーデック状態を確定
    // =========================================================
    auto* x           = new WmaInfo();
    x->buffer         = std::move(pcmBuffer);
    x->position       = 0;
    x->channels       = static_cast<int>(nChannels);
    x->sampleRate     = static_cast<int>(nSampleRate);
    x->bytesPerSample = fmt.bytesPerSample;
    x->fmodFormat     = fmt.fmodFormat;

    x->waveFormat.format       = fmt.fmodFormat;
    x->waveFormat.channels     = static_cast<int>(nChannels);
    x->waveFormat.frequency    = static_cast<int>(nSampleRate);
    x->waveFormat.pcmblocksize = static_cast<unsigned int>(nChannels) * fmt.bytesPerSample;
    x->waveFormat.lengthpcm    = totalPcmFrames;

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
    setTag(tagTitle,  "Title");
    setTag(tagArtist, "Artist");
    setTag(tagAlbum,  "Album");
    setTag(tagYear,   "YEAR");
    setTag(tagTrackNumber, "TRACKNUMBER");
    setTag(tagTrackTotal,   "TRACKTOTAL");

    if (!tagTrackNumber.empty())
    {
        const std::string trackValue = tagTrackTotal.empty()
            ? tagTrackNumber
            : tagTrackNumber + "/" + tagTrackTotal;
        setTag(trackValue, "TRACK");
    }

    return FMOD_OK;
}

// =========================================================
// close コールバック
// =========================================================
static FMOD_RESULT F_CALL wmaCodec_close(FMOD_CODEC_STATE* codec)
{
    delete reinterpret_cast<WmaInfo*>(codec->plugindata);
    codec->plugindata = nullptr;
    mfRelease();
    return FMOD_OK;
}

// =========================================================
// read コールバック
// =========================================================
static FMOD_RESULT F_CALL wmaCodec_read(FMOD_CODEC_STATE* codec,
    void* buffer, unsigned int sizebytes, unsigned int* bytesread)
{
    auto* x = reinterpret_cast<WmaInfo*>(codec->plugindata);
    const uint64_t bufSize = static_cast<uint64_t>(x->buffer.size());
    if (x->position >= bufSize)
    {
        *bytesread = 0;
        return FMOD_ERR_FILE_EOF;
    }
    const uint64_t remaining = bufSize - x->position;
    const unsigned int toCopy = static_cast<unsigned int>(
        std::min(
            static_cast<uint64_t>(sizebytes) *
                static_cast<uint64_t>(x->channels) *
                static_cast<uint64_t>(x->bytesPerSample),
            remaining));
    memcpy(buffer, x->buffer.data() + x->position, toCopy);
    x->position += toCopy;
    *bytesread   = toCopy;
    return FMOD_OK;
}

// =========================================================
// その他コールバック
// =========================================================
static FMOD_RESULT F_CALL wmaCodec_setposition(FMOD_CODEC_STATE* codec,
    int /*subsound*/, unsigned int position, FMOD_TIMEUNIT postype)
{
    auto* x = reinterpret_cast<WmaInfo*>(codec->plugindata);
    if (postype == FMOD_TIMEUNIT_PCMBYTES)
        x->position = std::min(static_cast<uint64_t>(position),
                               static_cast<uint64_t>(x->buffer.size()));
    return FMOD_OK;
}

static FMOD_RESULT F_CALL wmaCodec_getposition(FMOD_CODEC_STATE* codec,
    unsigned int* position, FMOD_TIMEUNIT postype)
{
    auto* x = reinterpret_cast<WmaInfo*>(codec->plugindata);
    if (postype == FMOD_TIMEUNIT_PCMBYTES)
        *position = static_cast<unsigned int>(x->position);
    return FMOD_OK;
}

static FMOD_RESULT F_CALL wmaCodec_getlength(FMOD_CODEC_STATE* codec,
    unsigned int* length, FMOD_TIMEUNIT lengthtype)
{
    auto* x = reinterpret_cast<WmaInfo*>(codec->plugindata);
    if (lengthtype == FMOD_TIMEUNIT_PCMBYTES)
        *length = static_cast<unsigned int>(x->buffer.size());
    return FMOD_OK;
}

static FMOD_RESULT F_CALL wmaCodec_soundcreated(FMOD_CODEC_STATE* /*codec*/,
    int /*subsound*/, FMOD_SOUND* /*sound*/)
{
    return FMOD_OK;
}

static FMOD_RESULT F_CALL wmaCodec_getwaveformat(FMOD_CODEC_STATE* codec,
    int index, FMOD_CODEC_WAVEFORMAT* waveformat)
{
    if (index != 0) return FMOD_ERR_FORMAT;
    auto* x    = reinterpret_cast<WmaInfo*>(codec->plugindata);
    *waveformat = x->waveFormat;
    return FMOD_OK;
}

// =========================================================
// プラグイン登録
// =========================================================
static FMOD_CODEC_DESCRIPTION s_wmaCodecDesc =
{
    FMOD_CODEC_PLUGIN_VERSION,
    "FMOD WMA Codec",
    0x00010000,
    1,
    FMOD_TIMEUNIT_PCMBYTES,
    &wmaCodec_open,
    &wmaCodec_close,
    &wmaCodec_read,
    &wmaCodec_getlength,
    &wmaCodec_setposition,
    &wmaCodec_getposition,
    &wmaCodec_soundcreated,
    &wmaCodec_getwaveformat
};

#ifdef __cplusplus
extern "C" {
#endif
    __declspec(dllexport) FMOD_CODEC_DESCRIPTION* F_API FMODGetCodecDescription()
    {
        return &s_wmaCodecDesc;
    }
#ifdef __cplusplus
}
#endif

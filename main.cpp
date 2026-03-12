#pragma comment(lib, "libfaad.lib")
#include <memory>
#include <iterator>
#include <vector>
#include <algorithm>
#include <string>
#include <unordered_map>
#include "neaacdec.h"
#include "fmod.h"

#pragma warning(push)
#pragma warning(disable:4018)
#pragma warning(disable:4101)
#pragma warning(disable:4267)
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
#pragma warning(pop)

typedef unsigned long       DWORD;
typedef int                 BOOL;
typedef unsigned char       BYTE;
typedef unsigned short      WORD;

typedef struct 
{
    std::byte size[4];
    std::byte header[4];
    std::vector<std::byte> data;
} MP4HEADER;

// FAAD2とFMODのフォーマット対応情報
typedef struct
{
    FMOD_SOUND_FORMAT  fmodFormat;      // FMODのフォーマット
    unsigned char      faadFormat;      // FAAD2のフォーマット
    unsigned int       bytesPerSample;  // 1サンプルあたりのバイト数
} PCMFormatInfo;

typedef struct 
{
    std::uint64_t sample_rates;
    std::byte channels;
    NeAACDecHandle aac;
    std::vector<std::byte> buffer;
    unsigned long bufferlen;

    std::uint64_t lengthpcm;
    std::uint32_t pcmblocks;
    std::uint32_t position;

    PCMFormatInfo          format;      // フォーマット情報

    std::vector<std::byte> title;
    std::vector<std::byte> artist;
    std::vector<std::byte> album;
} info;

// =========================================================
// フォーマット解決関数
// userexinfoからFMODのフォーマットを取得し、
// 対応するFAAD2フォーマットとバイト数を返す
// =========================================================
PCMFormatInfo resolvePCMFormat(const FMOD_CREATESOUNDEXINFO* userexinfo)
{
    // userexinfoが無いか、フォーマット未指定の場合はPCM16にフォールバック
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
        // 未対応フォーマットはPCM16にフォールバック
        return { FMOD_SOUND_FORMAT_PCM16, FAAD_FMT_16BIT, 2 };
    }
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

// openコールバック関数は、ファイルからデータを読み込んでデコードするために使用されます。
FMOD_RESULT F_CALLBACK myCodec_open(FMOD_CODEC_STATE* codec, FMOD_MODE usermode, FMOD_CREATESOUNDEXINFO* userexinfo)
{
    // ファイルからデータを読み込んで、state->plugindataに保存する

    if (!codec)
        return FMOD_ERR_INTERNAL;

    std::uint32_t totalSize = 0;
    codec->functions->size(codec, &totalSize);

    // 無いと思うけど読み込んだファイルがサイズ0以下の場合はEOF扱い
    if (totalSize <= 0)
        return FMOD_ERR_FILE_EOF;

    std::uint64_t totalRead = 0;    // 累計リードサイズ
    std::uint32_t readBytes = 0;    // リードサイズ

    // MP4 HEADER
    auto chunk = std::make_unique<MP4HEADER>();

    FMOD_RESULT r;
    std::uint64_t size = 0;         // データ領域のサイズ
    std::byte bytes[8];             // EXTEND SIZE

    // EXTEND SIZEフラグ
    std::byte extend[4] = { std::byte(0x00), std::byte(0x00), std::byte(0x00), std::byte(0x01) };

    // 先頭ブロック(8バイト)の読み込み
    r = codec->functions->read(codec, chunk->size, 4, &readBytes);
    r = codec->functions->read(codec, chunk->header, 4, &readBytes);

    // =========================================================
    // 先頭ブロックがftyp:M4Aの場合のみ、このコーデックは有効
    // =========================================================
    // ftypブロックの検出
    if (std::memcmp(chunk->header, "ftyp", 4) != 0)
        return FMOD_ERR_FORMAT; // フォーマットエラー

    // データ領域のサイズ取得
    size = _get_size(chunk->size);
    // データ領域確保(ヘッダブロックの8バイトは減算)
    chunk->data.resize(static_cast<size_t>(size - 8));

    // ftype:[M4A]　データ領域からファイルタイプの取得
    r = codec->functions->read(codec, chunk->data.data(), static_cast<unsigned int>(size - 8), &readBytes);
    if (std::memcmp(chunk->data.data(), "M4A ", 4) != 0
    &&  std::memcmp(chunk->data.data(), "mp42", 4) != 0)
        return FMOD_ERR_FORMAT; // フォーマットエラー

    bool mdat = false;
    bool moov = false;

    // API連携データ
    auto x = std::make_unique<info>();

    // mdatブロックまでループ
    while (!mdat)
    {
        // ヘッダブロック8バイト読み込み
        r = codec->functions->read(codec, chunk->size, 4, &readBytes);
        totalRead += readBytes;
        r = codec->functions->read(codec, chunk->header, 4, &readBytes);
        totalRead += readBytes;

        // サイズ領域がEXTEND SIZEフラグの場合
        if (std::memcmp(chunk->size, extend, 4) == 0)
        {
            // さらに8バイト読み込み
            r = codec->functions->read(codec, bytes, 8, &readBytes);
            totalRead += readBytes;
            // 8バイトがデータサイズとなる（ヘッダ16バイト分は減算）
            size = _get_size_64(bytes) - 16;
        }
        else {
            // 通常のデータサイズ取得
            size = _get_size(chunk->size) - 8;
        }
        // データ領域確保
        chunk->data.resize(static_cast<size_t>(size));
        // データ部読み込み
        r = codec->functions->read(codec, chunk->data.data(), static_cast<unsigned int>(size), &readBytes);
        totalRead += readBytes;

        // 累計読み込みサイズがファイルサイズ以上の場合はループ脱出
        if (totalRead >= totalSize)
            break;

        if (std::memcmp(chunk->header, "mdat", 4) == 0) 
        {
            // mdatのデータサイズ確保
            x->bufferlen = readBytes;
            x->buffer.clear();
            x->buffer.resize(x->bufferlen);
            // データ部をAPI連携データへ
            std::memcpy(x->buffer.data(), chunk->data.data(), x->bufferlen);
            mdat = true;
        }
    }

    // mdat見つからなかった場合はエラー
    if (!mdat)
        return FMOD_ERR_FORMAT; // フォーマットエラー

    // =========================================================
    // minimp4によるタグ情報取得
    // =========================================================
    // ファイル先頭に戻す
    codec->functions->seek(codec, 0, FMOD_CODEC_SEEK_METHOD_SET);

    MP4D_demux_t mp4 = {};
    if (MP4D_open(&mp4, mp4_read_callback, codec, totalSize) != 0)
    {
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
        MP4D_close(&mp4);
    }    
    
    // =========================================================
    // フォーマット解決
    // =========================================================
    x->format = resolvePCMFormat(userexinfo);

    // =========================================================
    // FAAD2によるAACデコード
    // =========================================================

    // FAAD2デコーダオープン
    if (!(x->aac = NeAACDecOpen()))
        return FMOD_ERR_INTERNAL;

    // FAAD2デコーダ初期化
    if (NeAACDecInit(x->aac, reinterpret_cast<unsigned char*>(x->buffer.data()), x->bufferlen, reinterpret_cast<unsigned long*>(&x->sample_rates), reinterpret_cast<unsigned char*>(&x->channels)) != 0)
    {
        // 失敗した場合はクローズしてエラー
        if (x->aac)
            NeAACDecClose(x->aac);
        return FMOD_ERR_INTERNAL;
    }

    // FAAD2コンフィグ（この辺のパラメータは理解できてない）
    NeAACDecConfigurationPtr config;
    /* Set configuration */
    config = NeAACDecGetCurrentConfiguration(x->aac);
    config->outputFormat = x->format.faadFormat;
    // userexinfoにサンプルレートの指定がある場合はそちらを優先
    if (userexinfo && userexinfo->defaultfrequency > 0)
        config->defSampleRate = userexinfo->defaultfrequency;
    else
        config->defSampleRate = static_cast<unsigned long>(x->sample_rates);

    NeAACDecSetConfiguration(x->aac, config);


    void* buf = NULL;               // デコードデータ
    unsigned long position = 0;     // データ部の現在位置
    std::uint64_t read = 0;         // デコード後のデータサイズ
    std::vector<std::byte> decoded; // 全デコードデータ
    decoded.clear();

    NeAACDecFrameInfo frameInfo;

    // データ部の現在位置がデータサイズ未満の間はループ
    while (position < x->bufferlen)
    {
        // データ部の現在位置からデコード
        buf = NeAACDecDecode(x->aac, &frameInfo, (unsigned char*)&x->buffer[position], x->bufferlen - position);

        // エラーの場合はファイル破損
        if (frameInfo.error != 0)
        {
            x->bufferlen = 0;
            return FMOD_ERR_FILE_BAD;
        }

        // bytesconsumedは恐らくデコードに成功したサイズ
        if (frameInfo.bytesconsumed > x->bufferlen)
        {
            x->bufferlen = 0;
        }
        else
        {
            // samplesが何を指してるか分からんがとりあえずゼロじゃないはず
            if (frameInfo.samples != 0)
            {
                // デコードデータが無い場合はエラー
                if (!buf)
                    return FMOD_ERR_INTERNAL;

                // デコードに成功したサイズは勿論ゼロより大きいはず
                if (frameInfo.bytesconsumed > 0)
                {
                    // 全デコードサイズの領域確保（追記型）
                    // 1フレームのバイト数 = サンプル数 × 1サンプルあたりのバイト数
                    const std::uint64_t frameBytes = frameInfo.samples * x->format.bytesPerSample;
                    decoded.resize(decoded.size() + frameBytes);

                    // とりあえず全デコードデータへ今回のデコード分を追記
                    std::memcpy(&decoded[read], buf, frameBytes);

                    // データ部の位置をシーク
                    position += frameInfo.bytesconsumed;

                    // 全デコード後のサイズを加算
                    read += frameBytes;
                }
            }
        }
    }
    // デコード後のサイズ
    x->bufferlen = static_cast<unsigned long>(read);
    // lengthpcm = 総バイト数 ÷ チャンネル数 ÷ 1サンプルあたりのバイト数
    x->lengthpcm = static_cast<std::uint32_t>(read / x->format.bytesPerSample / static_cast<unsigned int>(x->channels));

    // データ部の領域を使って全デコードデータをAPI連携させる為領域確保しなおし
    x->buffer.clear();
    x->buffer.resize(read);
    // デコードデータのコピー
    std::memcpy(x->buffer.data(), decoded.data(), read);

    // おまじない
    codec->numsubsounds = 0; //number of 'subsounds' in this sound.  For most codecs this is 0, only multi sound codecs such as FSB or CDDA have subsounds. 

    // API連携
    codec->plugindata = x.release();  // unique_ptrから所有権を手放してFMODへ渡す

    return FMOD_OK;
};

// closeコールバック関数は、デコードしたデータを解放するために使用されます。
FMOD_RESULT F_CALLBACK myCodec_close(FMOD_CODEC_STATE* codec)
{
    // デコードしたデータを解放する
    // デコーダはクローズ忘れずに
    if (codec->plugindata != nullptr)
    {
        info* x = (info*)codec->plugindata;
        if (x->aac)
            NeAACDecClose(x->aac);
        delete(x);
    }

    return FMOD_OK;
};

// readコールバック関数は、デコードしたデータを返すために使用されます。
FMOD_RESULT F_CALLBACK myCodec_read(FMOD_CODEC_STATE* codec, void* buffer, unsigned int sizebytes, unsigned int* bytesread)
{
    // API連携データの取得
    info* x = (info*)codec->plugindata;
    if (!x || !bytesread)
        return FMOD_ERR_INTERNAL;

    // 残りデータ量を計算
    std::uint64_t remaining = x->bufferlen - x->position;

    // 再生終了
    if (remaining == 0)
    {
        *bytesread = 0;
        return FMOD_ERR_FILE_EOF;
    }

    // 1サンプルのバイト数を計算

    // FMODの要求量と残量の小さい方だけコピー（オーバーランしない）
    unsigned int toCopy = static_cast<unsigned int>(
        std::min(static_cast<std::uint64_t>(sizebytes * static_cast<unsigned int>(x->channels) * x->format.bytesPerSample), remaining)
    );

    // デコードしたデータをbufferにコピーする
    // FMODのバッファ全体をゼロ初期化（末尾の無音保証）
    memset(buffer, 0, toCopy + 1);
    std::memcpy(buffer, &x->buffer[x->position], toCopy);

    // 再生位置の加算
    x->position += toCopy;

    // 今回読み込んだデータサイズ
    *bytesread = toCopy;

    return FMOD_OK;
};

// getlengthコールバック関数は、オーディオファイルの長さを返すために使用されます。
FMOD_RESULT F_CALLBACK myCodec_getlength(FMOD_CODEC_STATE* codec, unsigned int* length, FMOD_TIMEUNIT lengthtype)
{
    // 使わなかった
    return FMOD_OK;
};

// setpositionコールバック関数は、再生位置を設定するために使用されます。
FMOD_RESULT F_CALLBACK myCodec_setposition(FMOD_CODEC_STATE* codec, int subsound, unsigned int position, FMOD_TIMEUNIT postype)
{
    info* x = (info*)codec->plugindata;
    if (!x)
        return FMOD_ERR_INTERNAL;

    // 再生位置を設定する
    x->position = position;
    return FMOD_OK;
};

// getpositionコールバック関数は、再生位置を取得するために使用されます。
FMOD_RESULT F_CALLBACK myCodec_getposition(FMOD_CODEC_STATE* codec, unsigned int* position, FMOD_TIMEUNIT postype)
{
    info* x = (info*)codec->plugindata;
    if (!x)
        return FMOD_ERR_INTERNAL;

    // 再生位置を設定する
    *position = static_cast<unsigned int>(x->position);
    return FMOD_OK;
};

// soundcreatedコールバック関数は、サウンドが作成されたときに呼び出されます。
FMOD_RESULT F_CALLBACK myCodec_soundcreated(FMOD_CODEC_STATE* codec, int subsound, FMOD_SOUND* sound)
{
    // サウンドが作成されたときの処理
    return FMOD_OK;
};

FMOD_RESULT F_CALLBACK myCodec_getWaveFormat(FMOD_CODEC_STATE* codec, int index, FMOD_CODEC_WAVEFORMAT* waveformat)
{
    info* x = (info*)codec->plugindata;
    if (!x)
        return FMOD_ERR_INTERNAL;

    // PCMデータフォーマットの設定だと思う
    // 読み込んだAACデータを元に可変にしてあげるべきだろうけどやり方分からん
    waveformat->channels    = static_cast<int>(x->channels);
    waveformat->format      = x->format.fmodFormat;    // userexinfoに合わせたフォーマット
    waveformat->mode        = FMOD_DEFAULT;
    waveformat->frequency   = static_cast<int>(x->sample_rates);
    // この値ヘタにいじるとアクセス違反発生する
//  waveformat->pcmblocksize = static_cast<int>(x->channels) * 2;      //    2 = 16bit pcm 
//  waveformat->pcmblocksize = x->pcmblocks;
    // 曲の長さ
    waveformat->lengthpcm   = static_cast<unsigned int>(x->lengthpcm);// bytes converted to PCM samples ;

    // オーディオファイルの形式情報を取得する
    return FMOD_OK;
};

/*
    Codec structures

    typedef struct FMOD_CODEC_DESCRIPTION
    {
        unsigned int                      apiversion;
        const char* name;
        unsigned int                      version;
        int                               defaultasstream;
        FMOD_TIMEUNIT                     timeunits;
        FMOD_CODEC_OPEN_CALLBACK          open;
        FMOD_CODEC_CLOSE_CALLBACK         close;
        FMOD_CODEC_READ_CALLBACK          read;
        FMOD_CODEC_GETLENGTH_CALLBACK     getlength;
        FMOD_CODEC_SETPOSITION_CALLBACK   setposition;
        FMOD_CODEC_GETPOSITION_CALLBACK   getposition;
        FMOD_CODEC_SOUNDCREATE_CALLBACK   soundcreate;
        FMOD_CODEC_GETWAVEFORMAT_CALLBACK getwaveformat;
    } FMOD_CODEC_DESCRIPTION;
*/

// コーデックの情報
FMOD_CODEC_DESCRIPTION myCodec = {
    FMOD_CODEC_PLUGIN_VERSION,      // バージョン番号
    "FMOD MP4/AAC Codec",           // コーデックの名前
    0x00010000,                     // ドライバーのバージョン番号
    0,                              // Default As Stream
    FMOD_TIMEUNIT_PCMBYTES,         // Timeunit
    &myCodec_open,                  // openコールバック
    &myCodec_close,                 // closeコールバック
    &myCodec_read,                  // readコールバック
    &myCodec_getlength,             // getlengthコールバック
    &myCodec_setposition,           // setpositionコールバック
    &myCodec_getposition,           // getpositionコールバック
    &myCodec_soundcreated,          // soundcreatedコールバック
    &myCodec_getWaveFormat          // getWaveFormatExコールバック
};

/*
FMODGetCodecDescription is mandatory for every fmod plugin.  This is the symbol the registerplugin function searches for.
Must be declared with F_API to make it export as stdcall.
MUST BE EXTERN'ED AS C!  C++ functions will be mangled incorrectly and not load in fmod.
*/


#ifdef __cplusplus
extern "C" {
#endif
    __declspec(dllexport)  FMOD_CODEC_DESCRIPTION* F_API FMODGetCodecDescription()
    {
        return &myCodec;
    }

#ifdef __cplusplus
}
#endif
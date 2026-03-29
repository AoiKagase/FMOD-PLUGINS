# FMOD-PLUGINS
Windows版FMODライブラリで利用できるプラグイン集です。  
現在は非対応の形式に対応させるコーデックをメインに開発中です。  
Windows用なので全てVisualStudio2026でのプロジェクト構成となっています。  
  
This is a collection of plugins available for the Windows version of the FMOD library.  
We are currently focusing on developing codecs that support formats not currently supported.  
Since this is for Windows, all projects are configured using Visual Studio 2026.  

## codec_mp4
* MP4コンテナに格納されたAAC/ALACを再生するコーデックです。  
  This is a codec for playing AAC/ALAC files stored in an MP4 container, using FMOD library.<br>

## codec_ape
* Monkey's Audio (APE)形式を再生するコーデックです。  
  This is a codec for playing Monkey's Audio (APE) files.

## codec_tak
* Tom's lossless Audio Kompressor (TAK)形式を再生するコーデックです。  
  利用には別途`tak_deco_lib.dll`が必要です。  
  This is a codec that plays files in the Tom's Lossless Audio Kompressor (TAK) format.  
  You will need the `tak_deco_lib.dll` file separately to use this.

## codec_tta
* The True Audio (TTA)形式を再生するコーデックです。  
  This is a codec that plays the True Audio (TTA) format.
  
## codec_wv
* WavPack (WV)形式を再生するコーデックです。<br>
  This is a codec for playing WavPack (WV) files.

## codec_srla
* Soleil Rising Lossless Audio (SRLA)形式を再生するコーデックです。国産形式です。  
  This is a codec that plays the Soleil Rising Lossless Audio (SRLA) format. It is a Japanese-developed format.
* https://github.com/aikiriao/SRLA

#
利用に際してはFMODライブラリのドキュメントを参照してください。<br>
Please refer to the FMOD library documentation for details.<br>
<hr>
C# Example.<br>

```
public void LoadPlugins()
{
	uint handle;
	PLUGINTYPE plugintype;
	uint version;

	FmodSystem.setPluginPath(".\\Plugins");
	FmodSystem.loadPlugin("codec_mp4.dll", out handle, 100);
//	FmodSystem.getPluginInfo(handle, out plugintype, out version);
	return;
}
```

TODO: Optimize.

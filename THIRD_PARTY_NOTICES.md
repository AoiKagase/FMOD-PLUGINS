# Third-Party Notices

This repository contains original project code plus interfaces to third-party
libraries and SDK files. The root [`LICENSE`](H:\sourcecode\FMOD-AAC-CODEC\LICENSE) applies only to the
original code authored for this project unless a file states otherwise.

This document is a practical summary for this repository. It is not legal
advice, and it does not replace the original license texts of upstream
projects.

## External SDK headers

- FMOD SDK headers: `fmod.h`, `fmod_codec.h`, `fmod_common.h`, `fmod_dsp.h`,
  `fmod_dsp_effects.h`, `fmod_errors.h`, `fmod_output.h`
  - Copyright: Firelight Technologies Pty, Ltd.
  - Status: not covered by this repository's MIT license.
  - Action: these files are not redistributed in this repository. Build locally
    with `FMOD_SDK_DIR` pointing at an installed FMOD Studio API SDK.

## Codec dependency summary

| Codec | Dependency | Upstream license seen in local sources | Notes |
| --- | --- | --- | --- |
| `ape` | Monkey's Audio SDK / MACLib | BSD-3-Clause style | Preserve copyright and license notice when redistributing binaries or source. |
| `mp4` | FAAD2 | GPL per bundled `COPYING` | Do not treat `codec_mp4.dll` as MIT. Review whether your intended distribution is compatible before publishing artifacts. |
| `mp4` | Apple ALAC decoder files | Apache-2.0 headers present in imported codec files | Preserve notices and license text for imported ALAC sources. |
| `mp4` | minimp4 | Single-header third-party component | Confirm and ship the upstream license text for the exact version you import. |
| `opus` | libogg | BSD-style | Preserve upstream notice. |
| `opus` | opus | BSD-style | Preserve upstream notice. |
| `opus` | opusfile | BSD-style | Preserve upstream notice. |
| `srla` | SRLA | MIT | Compatible with MIT; preserve upstream notice. |
| `tak` | TAK SDK (`tak_deco_lib`) | Proprietary / vendor SDK terms | Distribution depends on the TAK SDK terms. Review before shipping. |
| `tta` | libtta++ | LGPL | This repository compiles imported `libtta.cpp` directly into the plugin. Do not treat `codec_tta.dll` as MIT-only. |
| `wv` | WavPack | BSD-3-Clause style | Preserve upstream notice. |
| `wma` | Windows Media Foundation / system components | Platform SDK / OS terms | Review Microsoft platform redistribution terms as needed. |

## Repository-specific cautions

- GitHub Actions currently builds and uploads `ape`, `mp4`, `opus`, `srla`,
  `tta`, `wma`, and `wv` DLL artifacts.
- `mp4` and `tta` need extra care before redistribution because their upstream
  licenses are not permissive MIT-style licenses.
- If you want a clean permissive-only distribution story, consider excluding
  `mp4` and `tta` from default artifact builds or replacing those dependencies.

## Upstream license locations found in the working tree

- Monkey's Audio: `deps/downloads/ape/License.txt`
- FAAD2: `deps/downloads/faad2/faad2-master/COPYING`
- ALAC: `deps/downloads/alac/alac-master/LICENSE`
- ALAC legacy Apple notice: `deps/downloads/alac/alac-master/codec/APPLE_LICENSE.txt`
- libogg: `deps/downloads/ogg/ogg-master/COPYING`
- opus: `deps/downloads/opus/opus-main/COPYING`
- opusfile: `deps/downloads/opusfile/opusfile-master/COPYING`
- SRLA: `deps/downloads/srla/SRLA-main/COPYING`
- libtta++: `deps/downloads/tta/libtta++-2.1/COPYING`
- WavPack: `deps/downloads/wavpack/WavPack-master/COPYING`

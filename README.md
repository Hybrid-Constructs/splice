# Splice

<img src="assets/icon.png" alt="Splice" width="128">

Takes two video files and combines them into a single **Hap Alpha** `.mov`:
the **RGB** comes from the first clip, the **alpha** from the luminance of the
second. Drag, drop, encode. Encoding runs entirely locally. The only network call is a version check at
startup (see *Update check* below).

Hap was chosen over DXV deliberately: it's an open, BSD-licensed codec (Vidvox),
Resolume/VDMX/Isadora play it natively, and FFmpeg has a real encoder for it, so
nothing here depends on a proprietary format.

## What it does

- Decodes both clips with FFmpeg (anything FFmpeg reads: mp4, mov, mkv, webm, …).
- Builds an RGBA frame per output frame: RGB from the colour clip, alpha derived
  from the matte clip.
- Encodes **Hap Alpha** (DXT5 textures, Snappy-compressed) into a QuickTime `.mov`.

The matte is rescaled to the colour clip's dimensions, so mismatched sources
still line up. The colour clip also defines the output frame rate. Alpha comes
from the matte's Rec.709 luminance, and encoding stops at the end of the
shorter clip.

RGB is **premultiplied against black**: masked-out areas are written as actual
black rather than colour hiding behind `alpha = 0`. That's what Resolume's
blending expects, and it avoids bright fringing on soft matte edges. It does
mean the output can't be reversed back to the original colour clip.

### Using it

**Join** — two zones, **RGB** and **A**. Drop one video on each and hit Render.

For batches, drop a **folder**, a **zip**, or a **multi-file selection** on
*either* zone. Files pair by the `<name>_rgb` / `<name>_a` convention
(case-insensitive) and each pair becomes `<name>_hap.mov`. The zones keep
showing a real pair — the first one at rest, and whichever is encoding during a
render — while a badge underneath counts the rest: *"12 pairs · 3 skipped"*.

Anything that doesn't pair (a lone `_rgb`, a file with no suffix) is skipped
rather than blocking the render.

**Split** — one zone. Drop any clips carrying alpha (Hap Alpha `.mov`, or
anything else FFmpeg decodes with an alpha channel) — singly, or as a
folder/zip/selection. Each becomes `<name>_rgb.mp4` + `<name>_a.mp4` (alpha as
greyscale), both H.264. Clips without alpha are skipped. The quality slider maps
to an x264 CRF of 28 (smallest) to 18 (best).

Split un-premultiplies, so a clip can go **Join → Split → Join** and come back
unchanged.

## Building

Needs CMake, Xcode command line tools, and a JUCE checkout. One-time setup:

```bash
brew install snappy x264 pkg-config   # build-time only; link statically
./tools/build-ffmpeg.sh               # ~10 min, static FFmpeg with Hap + libx264
git clone --depth 1 --branch 8.0.7 https://github.com/juce-framework/JUCE external/JUCE
```

Then:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
```

Output: `build/Splice_artefacts/Debug/Splice.app`.

JUCE is expected at `external/JUCE`. To reuse a checkout you already have, pass
`-DSPLICE_JUCE_DIR=/path/to/JUCE`.

### Why a custom FFmpeg build

Stock FFmpeg builds (Homebrew's included) ship the Hap *decoder* but not the
*encoder* — `hap_encoder` depends on external **libsnappy**, which most builds
omit. `tools/build-ffmpeg.sh` configures a minimal static FFmpeg with just the
Hap encoder and MOV muxer enabled. libsnappy links **statically**, so the built
app has no external dylib dependencies and runs on any machine:

```
$ otool -L "Splice.app/Contents/MacOS/Splice" | grep -v /usr/lib | grep -v /System
(nothing)
```

## Licence

**GPL-2.0-or-later**, because the splitter links libx264. See
[LICENSE.md](LICENSE.md) — this doesn't affect code signing.

## Notes and limitations

- **macOS / arm64.** FFmpeg's configure can't emit a universal binary in one
  pass, so the app is pinned to the host arch. Windows isn't wired up yet: the
  app code is portable, but `tools/build-ffmpeg.sh` is bash and FFmpeg's
  `configure` can't run natively there, so it needs an MSYS2 or prebuilt-libs
  path plus changes to the `-framework`/`brew` assumptions in `CMakeLists.txt`.
- Hap works on 4×4 texture blocks, so output dimensions are rounded **up** to a
  multiple of 4; the padding rows/columns are written fully transparent.
- Hap Alpha is DXT5, which is lossy block compression — alpha is quantised, so
  very soft gradient mattes will show some banding. That's inherent to the
  format, and it's what makes it cheap for the GPU to play back.
- Audio is not carried over; the output is video only.
- `juce::File::hasFileExtension` takes **bare** extensions (`"mp4;mov"`), not
  glob patterns (`"*.mp4;*.mov"`) — it's a suffix compare, not a wildcard match,
  so a glob string silently never matches. `FileChooser` wants the opposite
  (globs), so this file keeps both `videoExtensions` and `videoWildcards`. Mixing
  them up makes drag-and-drop fail while the file browser still works.
- No automated test suite (matching the other apps here); verified manually,
  see below.

## Verification

Encoded 60 frames of 1920×1080 h264 with a synthetic left-black / right-white
matte, then decoded the result:

```
codec        : hap
codec_tag    : Hap5          <- Hap Alpha fourcc
dimensions   : 1920x1080
pix_fmt      : rgba
LEFT-half mean alpha : 0     <- matte black  -> transparent
RIGHT-half mean alpha: 255   <- matte white  -> opaque
```

Confirming the RGB comes from the colour clip and the alpha from the matte.

## Update check

At launch Splice makes one anonymous `GET` to a public feed
(`https://api.hybridconstructs.com/v2/public/updates/splice`) that returns the
latest version number. No identifier, key or usage data is sent. If a newer
version exists, an "Update to x.y.z" button appears that opens the download page
in your browser. Nothing is downloaded or installed by the app. Any failure,
such as being offline, is silently ignored.

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Purpose

Splice converts between two representations of an alpha-carrying clip:

- **Join** — two videos into one **Hap Alpha** `.mov`: RGB from a "colour" clip,
  alpha from the luminance of a "matte" clip.
- **Split** — the reverse: one alpha-carrying clip into `<name>_rgb.mp4` +
  `<name>_a.mp4` (H.264).

It exists so a colour pass and a separately-rendered matte pass can be married
into one clip Resolume/VDMX play directly on the GPU — and taken apart again for
editing in tools that don't handle alpha.

Small, single-window, no services, no external runtime dependencies. The one
network call is the update check (see *Update check* below); encoding is fully
local.

This repo is public and self-contained: everything needed to build it is here or
fetched from public upstreams. Keep it that way, and keep internal infrastructure
details (signing, release pipeline, other repos) out of this file.

## Build

### macOS

```bash
brew install snappy x264 pkg-config   # build-time only (see gotchas)
./tools/build-ffmpeg.sh               # one-time, ~10 min
git clone --depth 1 --branch 8.0.7 https://github.com/juce-framework/JUCE external/JUCE
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
```

JUCE defaults to `external/JUCE` (gitignored); point `-DSPLICE_JUCE_DIR=` at any
other checkout to reuse one. Changing it on an existing build dir fails in
juceaide ("source ... does not match the source ... used to generate cache"),
because `build/JUCE` keeps the old path in its own sub-build cache: delete
`build/JUCE` and reconfigure.

`build-ffmpeg.sh` hard-fails if either the Hap or libx264 encoder didn't get
enabled, so a missing dependency can't silently produce a build that fails at
runtime instead. Note it needs `pkg-config` on PATH — without it the script
exits before configuring anything.

Output: `build/Splice_artefacts/Debug/Splice.app`. Debug is the default per
the workspace convention; single-config generator, so reuse the one `build/`.

### Windows

```powershell
git clone https://github.com/microsoft/vcpkg
.\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "$PWD\vcpkg"          # one-time per shell/session
cmake --preset windows
cmake --build build
```

FFmpeg comes from vcpkg's `ffmpeg` port (`vcpkg.json`), not from
`tools/build-ffmpeg.sh` — that script is bash and FFmpeg's `./configure` can't
run under MSVC. See *Architecture decisions → FFmpeg sourcing differs by
platform* below for why. `CMakePresets.json`'s `windows` preset wires up
Ninja + the vcpkg toolchain file + the `x64-windows-static` triplet in one
shot; the manual equivalent is
`cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static`.

Output: `build/Splice_artefacts/Debug/Splice.exe`.

**Verified so far:** configures, compiles, links (Debug, VS 2022 + vcpkg) and
launches; `dumpbin /dependents` shows only system DLLs (no FFmpeg or VC++
runtime). **Not yet verified:** the encode path on Windows - run the
*Verification* procedure below before trusting output.

No automated test suite on either platform (matching the other apps here) —
see *Verification* below for how the encode path was actually proven.

## Architecture decisions

### Hap, not DXV

DXV3 is Resolume-proprietary. Hap is open (BSD, Vidvox), plays natively in
Resolume/VDMX/Isadora, and — decisively — FFmpeg ships a real *encoder* for it,
so nothing has to be reverse-engineered. **Hap Alpha** specifically, because it
uses DXT5 textures, which is the variant that carries an alpha channel.

An earlier pass had gone some way into DXV3 (its frame header layout is readable
from FFmpeg's `libavcodec/dxv.c`, and the fourccs are visible in Arena's
binary). That was abandoned on purpose. Don't resurrect it.

### FFmpeg is vendored and built from source — on macOS

`tools/build-ffmpeg.sh` builds a minimal static FFmpeg into
`external/ffmpeg-install`. It is **not** interchangeable with a system or
Homebrew FFmpeg — see the libsnappy gotcha below.

The configure line disables all encoders/muxers and re-enables only
`hap` + `mov`, keeping the static libs small.

### FFmpeg sourcing differs by platform, on purpose

macOS builds FFmpeg from source via `./configure` (above). Windows instead
takes FFmpeg from **vcpkg**'s `ffmpeg` port (`vcpkg.json`, `snappy` + `x264` +
`gpl` features). This is an intentional asymmetry, not an inconsistency to
"fix":

- FFmpeg's `./configure` needs a POSIX-ish shell and can't run under MSVC.
  Building it via MSYS2/MinGW instead was considered and rejected: the app
  links with pure MSVC, and mixing MinGW-built static libs into an MSVC-linked
  executable risks a CRT/ABI mismatch that nothing here could prove out.
  vcpkg's port builds FFmpeg with MSVC itself, so the object code is
  ABI-compatible with the rest of the app.
- Consequence: vcpkg's port has no equivalent of `--disable-encoders` /
  `--disable-muxers`, so the Windows static libs carry FFmpeg's full default
  encoder/muxer set rather than the narrow `hap`+`mov`+`libx264`+`mp4` selection
  the macOS build hand-picks. Bigger static footprint on Windows; harmless
  otherwise since only the Hap/x264/mov/mp4 code paths are ever exercised.
- The Hap encoder itself isn't behind a vcpkg feature flag — it's in FFmpeg's
  default encoder set. It only needs `libsnappy` to actually be enabled, which
  is what the `snappy` feature in `vcpkg.json` supplies (vcpkg's port docs it
  explicitly as "needed for hap encoding").

### Threading and ownership

`HapEncoder` is a `juce::Thread`. All decode/encode work happens there; progress
and completion come back to the message thread via
`juce::MessageManager::callAsync`. The GUI never touches FFmpeg state.

`RgbaReader` (an implementation detail inside `HapEncoder.cpp`) wraps one file's
demux → decode → swscale chain so the encode loop stays readable. It hands out
tightly-packed RGBA frames in order via `next()`.

### Geometry and timing come from the colour clip

The colour clip defines output width, height and frame rate; the matte is
rescaled to match. This makes mismatched sources "just work" rather than being
rejected.

### Alpha is premultiplied against black

RGB is scaled by alpha before encoding, so masked-out areas are genuinely black
rather than full-brightness colour hiding behind `alpha = 0`. This is what
Resolume's `GL_ONE, GL_ONE_MINUS_SRC_ALPHA` blending expects — feeding it
straight (non-premultiplied) RGB produces bright fringing on soft matte edges.

It also compresses better: DXT5 stores colour and alpha separately, so leaving
arbitrary colour data in fully transparent regions wastes bitrate on pixels
nobody sees.

Consequence: the output is **not** round-trippable back to the original colour
clip — the RGB in masked regions is gone for good. That's intended.

### Split mode is the exact inverse of Join

`HapSplitter` reverses `HapEncoder`: it takes one alpha-carrying clip and writes
`<name>_rgb.mp4` (colour) + `<name>_a.mp4` (alpha as greyscale), both H.264.

The two directions are true inverses because the splitter **un-premultiplies**
(`c * 255 / a`) exactly where the encoder premultiplies (`c * a / 255`). Verified
by round-tripping — see *Verification*. If you change one side's premultiply,
change the other or the round-trip silently drifts.

Fully transparent pixels (`a == 0`) have no recoverable colour, so they're
written black rather than divided by zero.

The quality slider maps to an **x264 CRF** over 28..18 (inverted: slider 1.0 →
CRF 18). Outside roughly 18..28 CRF stops buying anything useful, so the slider
covers that band rather than the full 0..51 range.

### libx264 makes this GPL

MP4 output needs an H.264 encoder. libx264 was chosen over macOS's VideoToolbox
because it works on Windows too — but it's GPL, so the whole app is GPL. See
[LICENSE.md](LICENSE.md). This does **not** affect code signing. Dropping Split
mode would return the project to LGPL, since Hap needs only libsnappy.

### Batch: two zones showing one pair at a time

The **RGB + A** zones are the app's core affordance — they make the job legible
at a glance, so batch support was built around them rather than replacing them.
An earlier iteration collapsed both modes to a single zone; it worked but read
as abstract, and was reverted.

The model: **the zones display a pair, and a batch is just more than one pair.**
The zones always show real filenames — the first job at rest, and the job
currently encoding during a render (driven by `BatchRunner::onJobStarted`), so
you watch the batch move through. A small badge underneath carries only what the
zones can't: `"12 pairs · 3 skipped"`.

Drop routing (`MainComponent::handleDrop`):
- A **folder, zip, or multi-file** drop on *either* zone replaces the whole
  batch. There's deliberately no wrong place to drop a batch.
- A **single file** sets just the side it landed on, which is what preserves the
  original explicit RGB/A workflow. Two hand-assigned sides become a one-job batch.
- In Split mode everything is a batch, since there's only one input per job.

`JobScanner` flattens the drop (recursing into folders, extracting zips via
`juce::ZipFile` into a temp dir it owns) and buckets files by stem. A job needs
both halves; a lone `_rgb` or `_a` is skipped. Render processes what paired and
never blocks on a dialog.

Gotchas in the stem rule (`JobScanner::stemFor`):
- Matching is case-insensitive, so `CLIP_RGB` pairs with `clip_a`. The *bucket
  key* is lowercased too — keying on the raw stem silently split `UPPER_RGB`
  from `upper_a` into two half-pairs, reported as skipped. The first spelling
  seen is kept for naming the output.
- `_rgb` is tested before `_a` — order matters, since `my_rgb_a` is a *matte*
  whose stem is `my_rgb`.
- Bare `_rgb.mp4` / `_a.mp4` are rejected: an empty stem would bucket unrelated
  files together and name the output `_hap.mov`.
- Two files dropped one per zone (or two loose, unconventionally named files) are named by
  `JobScanner::stemForPair`: the colour file's name minus `_rgb`, else the matte's minus `_a`.
  Using the colour file's raw name made `clip_rgb` + `clip_a` write `clip_rgb_hap.mov`.
- Dotfiles (`.DS_Store`, `._` forks) are filtered, or a zip from macOS pollutes
  the job list.

### BatchRunner must not wait on the engines' callbacks

`HapEncoder`/`HapSplitter` deliver `onFinished` through
`juce::MessageManager::callAsync`. `BatchRunner` therefore polls
`isEncoding()`/`isSplitting()` instead of waiting on that callback — waiting
would block the batch thread until the message thread ran, and `~BatchRunner`
(invoked *from* the message thread) would deadlock against it.

Success is read from the output file's existence, since the engines delete their
output on failure. If you ever make the engines signal completion directly, this
polling can go — but don't reintroduce a wait on a `callAsync` callback.

### The UI exposes only what's needed

Two square drop zones (`RGB` and `A`), a mode tab pair, a badge, and a Render
button. Split mode hides the matte zone and reveals the quality slider.

Empty zones draw an accent-blue down arrow over their letter, the same arrow
(48-unit path, stroke 5) as the app sketch on the Splice web page; keep the two
alike. Render uses `GreyWhenDisabledLookAndFeel`, so while disabled it's solid
panel grey rather than JUCE's stock half-alpha accent, which read as a faded
version of the live button.

`HapEncoder::Settings` still carries `matteChannel`, `lengthMode` and
`invertMatte`, which simply take their defaults (luminance / shortest / no
invert). The encoder API is deliberately left intact so those can be re-surfaced
without touching the encode path.

**Design note:** resist adding text. Every attempt to explain state in words has
been replaced by making the zones show the actual filenames instead, which is
both smaller and clearer. The badge is the one concession, and it's limited to
counts.

## App icon

Same split idea as the other Hybrid Constructs app icons: the square is two halves (app dark `#1b1d21`
left, Splice blue `#4da3ff` right) with the down arrow (the one the empty drop
zones draw) in the opposite color on each half. Two renderings of the same
artwork, picked by `CMakeLists.txt` per platform: `assets/icon-macos.png` is a
squircle on an 824px body in the 1024 canvas with a soft shadow, and
`assets/icon-windows.png` fills the canvas with rounded corners. The `.svg` next
to each is the source; re-render the PNG from it at 1024x1024 with a transparent
background.

## Update check

`UpdateChecker` makes a single anonymous `GET` to the public feed
`/v2/public/updates/splice` on the Hybrid Constructs API (`UpdateConfig::feedUrl`)
and compares the returned version with the running one (`isNewerVersion`, plain
dotted-integer compare). If newer, `MainComponent` shows an "Update to x.y.z"
button in the tab row that opens `UpdateConfig::downloadUrl` in the browser. It
never downloads or installs anything.

- The app is open source, so it carries **no API key**. The feed is deliberately
  unauthenticated and returns only a version number, with no URLs from the server
  that the app follows. Keep it that way: don't add a key, and don't make the app
  open a URL taken from the response.
- **`downloadUrl` is the Splice page, `https://hybridconstructs.com/splice`.**
  Emptying it switches the check off: no request, no button.
- Failures (offline, timeout, bad JSON, 404 before a version is published) all
  read as "no update"; there is no error UI by design.
- The running version comes from `JUCE_APPLICATION_VERSION_STRING`, i.e. the CMake
  `project(VERSION)`. Bump it there; `Main.cpp` no longer hardcodes it.
- `-DSPLICE_BUILD_NUMBER=N` appends a fourth component (`1.0.0.N`) to the bundle/file
  version and to `JUCE_APPLICATION_VERSION_STRING`. The compare treats a missing
  component as 0, so `1.0.0` is not newer than `1.0.0.N`, and `1.0.1` is. Release
  builds set it; tags and `project(VERSION)` stay three-part.
- The check runs on a thread; the destructor cancels the in-flight `WebInputStream`
  so quitting never waits on a slow connection.

## Critical gotchas

### `hasFileExtension` takes bare extensions, not globs

`juce::File::hasFileExtension("*.mp4")` **never matches**. It's a suffix
compare (`juce_File.cpp`, `File::hasFileExtension`),
not a wildcard match — it accepts `"mp4"` or `".mp4"`, and a leading `*` breaks it.

`juce::FileChooser` wants the exact opposite (glob patterns). So
`MainComponent.cpp` deliberately keeps **two** constants — `videoExtensions`
(bare) and `videoWildcards` (globs). Mixing them up produces a nasty
failure mode: drag-and-drop silently refuses every file while the file browser
still works perfectly, because `isInterestedInFileDrag` just quietly returns
false. This cost real debugging time; don't collapse the two constants.

### The Hap *encoder* needs external libsnappy

Stock FFmpeg builds — Homebrew's included — ship the Hap **decoder** but not the
**encoder**. In FFmpeg's `configure`: `hap_encoder_deps="libsnappy"`, and most
builds omit it. Without it you get `CONFIG_HAP_ENCODER 0` and
`avcodec_find_encoder_by_name("hap")` returns null at runtime.

`tools/build-ffmpeg.sh` hard-fails if the flag didn't come out as 1, so this
can't silently regress.

### libsnappy must link statically

`find_library(... NAMES snappy)` resolves to Homebrew's **dylib**, and the
bundle then refuses to launch on any machine without Homebrew. CMake asks for
`libsnappy.a` by name for this reason. Verify with:

```bash
otool -L "build/Splice_artefacts/Debug/Splice.app/Contents/MacOS/Splice" \
  | grep -v /usr/lib | grep -v /System     # must print nothing
```

Note CMake caches `SNAPPY_LIBRARY`; after changing that logic, clear it with
`cmake -B build -U SNAPPY_LIBRARY` or the old dylib path silently persists.

### macOS build is single-arch only

FFmpeg's `configure` can't emit a universal binary in one pass, so the static
libs are host-arch only and the app target is pinned to match via
`OSX_ARCHITECTURES`. A universal build would need two FFmpeg trees and a
`lipo` step. This doesn't apply to the Windows build - vcpkg's
`x64-windows-static` triplet is a fixed architecture already, nothing to pin.

### Windows support

Wired up via vcpkg rather than mirroring the macOS `build-ffmpeg.sh` approach
(see *FFmpeg sourcing differs by platform, on purpose* above for why). Nothing
in the app code is platform-specific (JUCE, FFmpeg, libx264 and libsnappy all
build fine on Windows); the divergence is entirely in how FFmpeg's static libs
get built, contained to `vcpkg.json` and the `WIN32`/`APPLE` branches in
`CMakeLists.txt`. Dropping video support on Windows isn't an option, since
decoding *is* the app, and prebuilt MinGW libs were rejected for the CRT/ABI
reason above.

### Hap requires 4×4 blocks

Output dimensions are rounded **up** to a multiple of 4; the padding rows and
columns are written fully transparent. Sources whose dimensions aren't multiples
of 4 are handled rather than rejected.

### DXT5 alpha is lossy

Hap Alpha is block compression, so alpha is quantised — very soft gradient
mattes band. Inherent to the format, not a bug in the encoder, and the reason
playback is cheap on the GPU.

## Releases

Official binaries are built from tagged commits of this repository. Release
notes should name the tag a binary was built from, so the corresponding source
is always identifiable (see [LICENSE.md](LICENSE.md)).

`.github/workflows/ci.yml` is a build-and-lint check only: GitHub-hosted runners,
read-only token, no secrets, no signing, no publishing. Don't add secrets,
self-hosted runners, `pull_request_target`, or private submodules/URLs here.

CI builds both platforms: macOS (`build`) and Windows (`build-windows`, vcpkg via
the `windows` preset on the runner's preinstalled vcpkg, binary cache keyed on
`vcpkg.json`). Neither has run on GitHub yet.

## Verification

There's no test suite, so the encode path was proven empirically and should be
re-proven after changes to `HapEncoder`:

Encode 60 frames of real 1080p footage with a synthetic left-black /
right-white matte, then decode the result and sample alpha per half:

```
codec        : hap
codec_tag    : Hap5          <- Hap Alpha fourcc (not Hap1/HapY)
pix_fmt      : rgba
LEFT  (matte black): max RGB 0,  mean alpha 0    <- transparent AND black
RIGHT (matte white): mean RGB 20, mean alpha 255  <- opaque, colour preserved
```

Checking **max** RGB (not mean) on the masked half is what catches a broken
premultiply: a stray bright pixel behind `alpha = 0` is invisible to a mean.
Before premultiplication that half read `mean RGB 20, max RGB 255`.

For the splitter, the check that matters is the **round-trip**: split a Hap
Alpha file, then re-join the two mp4s and confirm the result matches the
original. It caught nothing when written, but it's the only test that exercises
premultiply and un-premultiply against each other:

```
original  : LEFT max RGB 0, alpha 0  | RIGHT mean RGB 20, alpha 255
roundtrip : LEFT max RGB 0, alpha 0  | RIGHT mean RGB 20, alpha 255
```

The intermediate `_a.mp4` should read LEFT 0 / RIGHT 255 — if it reads mid-grey,
alpha is being written as luma-scaled video rather than raw values.

Checking `codec_tag` is `Hap5` matters: a misconfigured `format` option silently
produces plain Hap (`Hap1`, no alpha) and everything else still looks fine.
Sampling both halves is what actually proves alpha is routed from the *matte*
and not from the colour clip — using one clip for both inputs can't distinguish
the two.

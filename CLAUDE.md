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

Not currently part of the `hybrid.code-workspace` roots; it's a standalone tool
that borrows the JUCE checkout from the sibling `hybridffgl` repo.

## Build

```bash
brew install snappy x264 pkg-config   # build-time only (see gotchas)
./tools/build-ffmpeg.sh               # one-time, ~10 min
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
```

`build-ffmpeg.sh` hard-fails if either the Hap or libx264 encoder didn't get
enabled, so a missing dependency can't silently produce a build that fails at
runtime instead. Note it needs `pkg-config` on PATH — without it the script
exits before configuring anything.

Output: `build/Splice_artefacts/Debug/Splice.app`. Debug is the default per
the workspace convention; single-config generator, so reuse the one `build/`.

No automated test suite (matching the other apps here) — see *Verification*
below for how the encode path was actually proven.

## Architecture decisions

### Hap, not DXV

DXV3 is Resolume-proprietary. Hap is open (BSD, Vidvox), plays natively in
Resolume/VDMX/Isadora, and — decisively — FFmpeg ships a real *encoder* for it,
so nothing has to be reverse-engineered. **Hap Alpha** specifically, because it
uses DXT5 textures, which is the variant that carries an alpha channel.

An earlier pass had gone some way into DXV3 (its frame header layout is readable
from FFmpeg's `libavcodec/dxv.c`, and the fourccs are visible in Arena's
binary). That was abandoned on purpose. Don't resurrect it.

### FFmpeg is vendored and built from source

`tools/build-ffmpeg.sh` builds a minimal static FFmpeg into
`external/ffmpeg-install`. It is **not** interchangeable with a system or
Homebrew FFmpeg — see the libsnappy gotcha below.

The configure line disables all encoders/muxers and re-enables only
`hap` + `mov`, keeping the static libs small. This mirrors the approach in
[hybridffgl's testbed](../hybridffgl/tools/testbed/CMakeLists.txt), which builds
FFmpeg the same way via `ExternalProject` (different codec set: `mpeg4`/`mp4`).

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

`HapEncoder::Settings` still carries `matteChannel`, `lengthMode` and
`invertMatte`, which simply take their defaults (luminance / shortest / no
invert). The encoder API is deliberately left intact so those can be re-surfaced
without touching the encode path.

**Design note:** resist adding text. Every attempt to explain state in words has
been replaced by making the zones show the actual filenames instead, which is
both smaller and clearer. The badge is the one concession, and it's limited to
counts.

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
- **`downloadUrl` empty means the check is off:** no request, no button. Set it
  to switch the feature on.
- Failures (offline, timeout, bad JSON, 404 before a version is published) all
  read as "no update"; there is no error UI by design.
- The running version comes from `JUCE_APPLICATION_VERSION_STRING`, i.e. the CMake
  `project(VERSION)`. Bump it there; `Main.cpp` no longer hardcodes it.
- The check runs on a thread; the destructor cancels the in-flight `WebInputStream`
  so quitting never waits on a slow connection.

## Critical gotchas

### `hasFileExtension` takes bare extensions, not globs

`juce::File::hasFileExtension("*.mp4")` **never matches**. It's a suffix
compare ([juce_File.cpp:694](../hybridffgl/submodules/JUCE/modules/juce_core/files/juce_File.cpp#L694)),
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

### Single-arch only, and macOS-only for now

FFmpeg's `configure` can't emit a universal binary in one pass, so the static
libs are host-arch only and the app target is pinned to match via
`OSX_ARCHITECTURES`. A universal build would need two FFmpeg trees and a
`lipo` step. Same constraint the hybridffgl testbed documents.

**Windows is not yet wired up.** Nothing in the app code is macOS-specific —
JUCE, FFmpeg, libx264 and libsnappy all build there — but `tools/build-ffmpeg.sh`
is bash and FFmpeg's `configure` can't run natively on Windows. It needs either
an MSYS2 path or prebuilt static libs, plus replacing the `-framework`/`brew`
assumptions in `CMakeLists.txt`.

**There is no recipe to copy from the other repos.** hybridffgl's testbed wraps
its whole FFmpeg block in `if(NOT WIN32)` and drops video support on Windows —
fine there, because the testbed still loads plugins without it, but useless here
since decoding *is* the app. The `vcpkg_acquire_dependencies()` call in
hybridffgl's root `CMakeLists.txt` is vestigial: it's guarded on a cache
variable that is never set. Pulse vendors a prebuilt SDK (Steinberg ASIO), which
is the closest precedent for the prebuilt-libs route.

### Hap requires 4×4 blocks

Output dimensions are rounded **up** to a multiple of 4; the padding rows and
columns are written fully transparent. Sources whose dimensions aren't multiples
of 4 are handled rather than rejected.

### DXT5 alpha is lossy

Hap Alpha is block compression, so alpha is quantised — very soft gradient
mattes band. Inherent to the format, not a bug in the encoder, and the reason
playback is cheap on the GPU.

## Releases

`.github/workflows/build-release.yml` calls the shared
[build-standalone-app](../github-actions/CLAUDE.md) workflow with
`skip-dmg: true` and `skip-installer: true`. That's the **portable app** model:
the workflow zips the signed `.app` and `.exe` directly instead of building a
DMG or a WiX bundle, so no installer assets live in this repo. Signing still
happens — Azure Trusted Signing on Windows, codesign + notarytool on macOS.

This suits the app: one window, no file associations, no updater, nothing to
register with the OS. If it ever needs those, drop the two flags and add the
`Installer/` tree the other apps have.

GPL compliance for published binaries is satisfied by the repo being public;
release notes should name the tag a binary was built from. See
[LICENSE.md](LICENSE.md).

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

## Connections

- Borrows the JUCE checkout from [hybridffgl](../hybridffgl/CLAUDE.md)
  (`submodules/JUCE`) rather than cloning a second copy; override with
  `-DSPLICE_JUCE_DIR=`.
- The FFmpeg-from-source + static-link + host-arch-pinning pattern is shared
  with hybridffgl's [FFGL Testbed](../hybridffgl/tools/testbed/CMakeLists.txt),
  which is the reference implementation for it in this constellation.
- Produces clips intended for the same Resolume pipeline that
  [chaser](../chaser/CLAUDE.md) and [stageflow](../stageflow/CLAUDE.md) target.

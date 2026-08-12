# ffgl-datamosh

Datamosh effects for Resolume, as FFGL plugins for Windows and macOS.

**📖 [Documentation](https://legofsalmon.github.io/ffgl-datamosh/)** ·
[Parameters](https://legofsalmon.github.io/ffgl-datamosh/parameters.html) ·
[Recipes](https://legofsalmon.github.io/ffgl-datamosh/recipes.html) ·
[Architecture](https://legofsalmon.github.io/ffgl-datamosh/architecture.html) ·
[Download](https://github.com/legofsalmon/ffgl-datamosh/releases)

| Plugin | Type | Where it shows up |
| --- | --- | --- |
| **Datamosh** | `FF_EFFECT`, `DMSH` | Effects list. Drop it on a layer, a group, or the composition. |
| **Datamosh Transplant** | `FF_MIXER`, `DMMX` | A layer's blend-mode dropdown. Applies that layer's motion to the layers below. |

## Why it simulates rather than corrupts

Datamoshing works by breaking inter-frame compression — deleting I-frames so
pixels persist, or duplicating P-frames so motion vectors keep applying to an
increasingly wrong image.

**None of that is reachable inside Resolume.** Its playback path uses HAP and
DXV, which are all-intra codecs: every frame is an independent compressed still,
so there are no motion vectors anywhere to corrupt. Even with an H.264 source, an
FFGL plugin receives a decoded texture, never the bitstream.

So this estimates the motion itself and reproduces what a decoder that has lost
its keyframe does with it. For live use that trades up: it works on any source
including cameras and NDI, it cannot desync mid-set, and every stage becomes a
parameter instead of an encoder setting.

Melt, bloom and pixel drag all come out of one pipeline, and a **Damage** group
exists to get the motion wrong on purpose — because an accurate estimator
reconstructs the frame, which is correct and far too clean to read as a broken
codec. [The docs explain the rest.](https://legofsalmon.github.io/ffgl-datamosh/)

## Install

Grab an archive from [Releases](https://github.com/legofsalmon/ffgl-datamosh/releases),
copy both plugins into `Documents/Resolume Arena/Extra Effects`, restart
Resolume. Each archive carries an `INSTALL.txt`.

**macOS**: the bundles are unsigned, so Gatekeeper quarantines them and Resolume
silently will not load them. Run `xattr -dr com.apple.quarantine` on each bundle
after copying, or build from source, which avoids it.

## Build

CMake 3.20+ and a C++17 compiler. The FFGL SDK is vendored as a submodule and
GLEW ships prebuilt inside it, so there is no package manager involved.

```sh
git clone --recursive https://github.com/legofsalmon/ffgl-datamosh.git
cd ffgl-datamosh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

macOS builds a universal binary by default (`arm64;x86_64`); Resolume 7.11+ runs
natively on Apple Silicon and will not load an x86_64-only plugin.

Install straight into Resolume with
`-DDATAMOSH_INSTALL_TO_RESOLUME=ON` and `cmake --build build --target install`.

## Test

```sh
ctest --test-dir build --output-on-failure   # 32 tests
./build/tests/datamosh_tests --profile        # per-pass GPU timing
```

The suite creates a real headless OpenGL 4.1 core context and drives the
pipeline exactly as Resolume does, against synthetic clips with **known ground
truth** — a pattern translating at a known velocity must produce the matching
vectors. That is the only way to regression-test a motion estimator; "does it
look right" is not a test.

The Linux path uses EGL on a software rasteriser, so it runs on a CI machine
with no GPU and no display server.

## Release

Push a tag. `.github/workflows/release.yml` builds both platforms, re-runs the
tests, verifies the entry point and the macOS architectures, and publishes a
GitHub Release with the archives attached.

```sh
git tag v0.2.0 && git push origin v0.2.0
```

A release can also be cut from **Actions → release → Run workflow** by entering
the version there; the Releases API creates the tag against the chosen commit.
Running it with the version left empty builds and packages without publishing,
which is the way to test a change to the packaging itself.

## Status — not yet validated in Resolume

Everything is verified by the test suite; none of it is verified against the
actual host. The checklist that still needs a machine with Resolume on it:

- [ ] Both plugins appear and load, with their thumbnails
- [ ] Parameters render as collapsible groups (needs Resolume 7.3+)
- [ ] Style presets move the sliders in the panel, and survive pressing Trigger
- [ ] Find out what Resolume does with the mixer's inherited `mixVal` slider
- [ ] Composition resolution change while running
- [ ] Clip cut, layer bypass, rapid parameter scrubbing
- [ ] Two instances stacked; mixer appears in the blend-mode dropdown
- [ ] No VRAM growth over a 30-minute soak
- [ ] Confirm Resolume delivers `SetTime` to effects — the frame-advance gate in
      `src/plugins/DatamoshPlugin.h` assumes it does and falls back to advancing
      every call if not
- [ ] `--profile` on real hardware, then tune `SEARCH_LAMBDA`,
      `SEARCH_ZERO_BIAS`, `THRESHOLD_PIXEL_RANGE` (`src/core/MoshPipeline.cpp`)
      and the cut-sensitivity curve (`src/core/shaders/Control.glsl`) against
      footage

## Licence

Not yet chosen — which currently means all rights reserved, so nobody can
legally redistribute or modify it. Worth deciding before promoting the release.

The plugins link the FFGL SDK and (on Windows) GLEW, both BSD-style; their
notices are generated into a `THIRD-PARTY.txt` inside every release archive.

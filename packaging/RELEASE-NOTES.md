Two FFGL plugins that simulate datamoshing in Resolume, over a shared render core.

| Plugin | Where it shows up |
| --- | --- |
| **Datamosh** | Effects list — a layer, a group, or the composition |
| **Mosh Transplant** | A layer's blend-mode dropdown — applies that layer's motion to the layers below |

**📖 [Documentation](https://datamosh.letissier.ie)** · [Parameter reference](https://datamosh.letissier.ie/parameters.html) · [Recipes](https://datamosh.letissier.ie/recipes.html)

---

## This is a pre-release — read this first

Everything here is verified by an automated test suite: 40 tests running a real
headless OpenGL 4.1 context against synthetic footage with known ground truth,
green on Linux, Windows and macOS.

**It now loads and runs in Resolume.** The smoke test passed on a real host:
both plugins register and instantiate, the effect renders, Resolume delivers
parameter writes, the motion estimator is genuinely running, and wide-range
parameters arrive at their real values rather than normalised.

What has *not* been done is the rest of the pass — the aesthetic sweeps, a
30-minute soak, GL-state hygiene in a chain, and the mixer beyond instantiation.
The tuning defaults have still never been judged against real footage. Treat
this as something to try and report back on, not something to put in a show
without rehearsing first.

## Install

Unzip and copy the plugins into `Documents/Resolume Arena/Extra Effects`, then
restart Resolume. Full instructions are in the `INSTALL.txt` inside each archive.

**macOS users:** the bundles are not signed or notarised, so macOS quarantines
them and Resolume silently will not load them — the plugins just never appear.
Clear it with:

```sh
xattr -dr com.apple.quarantine ~/Documents/Resolume\ Arena/Extra\ Effects/Datamosh.bundle
xattr -dr com.apple.quarantine ~/Documents/Resolume\ Arena/Extra\ Effects/DatamoshTransplant.bundle
```

**Windows users:** nothing to do. Earlier notes told you to right-click each DLL
and tick *Unblock* — that applies to .NET plugins, not to these; `LoadLibrary`
never reads the mark. If the plugins do not appear, check whether **Smart App
Control** is on, which rejects unsigned DLLs silently.

Requires Resolume 7.3+. macOS builds are universal (arm64 + x86_64), so Apple
Silicon is native.

## Why this simulates rather than corrupts

Datamoshing works by breaking inter-frame compression — deleting I-frames so
pixels persist, or duplicating P-frames so motion vectors keep applying to an
increasingly wrong image.

None of that is reachable inside Resolume. Its playback path uses HAP and DXV,
which are all-intra codecs: every frame is an independent compressed still, so
there are no motion vectors anywhere to corrupt. Even with an H.264 source, an
FFGL plugin receives a decoded texture, never the bitstream.

So this estimates the motion itself and reproduces what a decoder that has lost
its keyframe does with it. For live use that trades up — it works on any source
including cameras and NDI, it cannot desync mid-set, and every stage becomes a
parameter instead of an encoder setting.

## What you get

All three classic looks from one pipeline:

- **Melt** — pixels stop refreshing while motion keeps flowing. With **Auto Mode
  → On Cut** on the composition, every clip change melts by itself.
- **Bloom** — freeze the vector field and motion piles up into flowing trails.
- **Pixel drag** — raise the threshold so only fast-moving blocks smear.

**Softness** carries it from hard macroblock tearing to smooth liquid flow.
**Style** presets set a coherent group of parameters from one dropdown.

A **Damage** group exists to get the motion wrong on purpose — Motion Lag, Block
Repeat, Quantise — because an accurate estimator reconstructs the frame, which
is correct and far too clean to read as a broken codec.

Beat sync via Resolume's clock, and audio-reactive triggering via FFT.

## Known gaps

- Only the smoke test has been run against a real Resolume. The full pass —
  aesthetic sweeps, soak, GL hygiene, the mixer — has not.
  [VALIDATING.md](https://github.com/legofsalmon/ffgl-datamosh/blob/main/VALIDATING.md)
  is the procedure and `tools/collect-datamosh-diagnostics.{sh,ps1}` gathers the
  evidence. Reports welcome.
- **Hold** needs a key or MIDI mapping with Resolume's **Piano** option ticked
  to be a held control at all; the panel toggle latches, because a mouse cannot
  hold a checkbox down.
- macOS bundles are unsigned and unnotarised.
- The mixer carries an inherited `mixVal` slider that does nothing — use its own
  **Mix** parameter. It cannot be renamed away without making the plugin
  unloadable; the reason is documented in the architecture page.
- Performance has only been measured on a software rasteriser. Run
  `datamosh_tests --profile` on real hardware before committing to 4K.

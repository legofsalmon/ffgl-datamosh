Two FFGL plugins that simulate datamoshing in Resolume, over a shared render core.

| Plugin | Where it shows up |
| --- | --- |
| **Datamosh** | Effects list — a layer, a group, or the composition |
| **Mosh Transplant** | A layer's blend-mode dropdown — applies that layer's motion to the layers below |

**📖 [Documentation](https://datamosh.letissier.ie)** · [Parameter reference](https://datamosh.letissier.ie/parameters.html) · [Recipes](https://datamosh.letissier.ie/recipes.html)

---

## 1.0.0 — the first licensed release

Datamosh now needs a licence key or a free trial from
[letissier.ie](https://letissier.ie) (see **Licence** below). The version jumps
from 0.3.0 to 1.0.0; the plugin ids are unchanged, so compositions saved with
0.x open as before. Nothing about the effect itself changed between 0.3.0 and
1.0.0.

### Where it stands

Everything here is verified by an automated test suite: 107 tests running a real
headless OpenGL 4.1 context against synthetic footage with known ground truth,
green on Linux, with Windows and macOS builds checked on every change.

**It loads and runs in Resolume.** The smoke test passed on a real host: both
plugins register and instantiate, the effect renders, Resolume delivers
parameter writes, the motion estimator is genuinely running, and wide-range
parameters arrive at their real values rather than normalised.

What has *not* been done is the rest of the pass — the aesthetic sweeps, a
30-minute soak, GL-state hygiene in a chain, and the mixer beyond instantiation.
The tuning defaults have still never been judged against real footage. Rehearse
with it before you put it in a show.

### What is new in 1.0.0

- **Licence keys and a free trial.** A new last parameter, **Licence**, on both
  plugins. Details below.
- **Send Feedback.** A button in a new **Help** section opens the feedback page
  in your browser, with the product and version already filled in.
- **Crash reports — off unless you turn them on.** If Resolume closes while
  Datamosh is in the middle of a frame, the next launch notices and the Licence
  field asks once whether to send a report: type `send` or `discard`.
  `always send` turns automatic reports on; `reports off` turns them off. A
  report never holds your licence key, email, name, file names or anything from
  your composition, and `README.txt` in the licence folder shows exactly what it
  does hold.
- **An error inside the plugin no longer reaches Resolume.** It is caught, the
  frame is passed through, and one `datamosh: error in ...` line goes to the log.
- **A frame size the GPU cannot allocate** is no longer retried on every frame;
  the plugin passes through and tries again a couple of seconds later.

### Which version am I running?

macOS: Get Info on the bundle. Windows: Properties → Details. Both platforms:
the end of the effect's description in Resolume, and the `datamosh:` line in
Resolume's log.

The version is deliberately *not* in the plugin name. `Mosh Transplant` already
uses all 15 characters FFGL allows, and Resolume addresses an effect by its
name — a name that changed every release would be a saved composition that
quietly failed to find its effect after an upgrade.

### Coming from 0.1.x

0.2.0 remapped several sliders so their travel does something across its whole
length — Mosh Amount, Decay, Motion Threshold and Motion Smoothing — and merged
Freeze into Motion Smoothing. A composition saved against 0.1.x will look
different at the same slider positions. The full list is in the changelog.

## Install

**macOS — use the `.pkg`.** Double-click it and follow the prompts. It is signed
and notarised, so there is no security warning, no Terminal, and no password —
everything installs into your own Documents folder. Tick Arena, Avenue or both,
and quit Resolume first, since it only scans for plugins at startup.

The zip is still there if you prefer copying files by hand, but it needs the
quarantine flag cleared afterwards — see the `INSTALL.txt` inside it. That step
is the whole reason the installer exists: macOS quarantines anything extracted
from a downloaded archive, and Resolume then declines to load the plugins with
no error at all.

**Windows — unzip and copy** the DLLs into `Documents\Resolume Arena\Extra
Effects`, then restart Resolume. Nothing else. Earlier notes told you to
right-click each DLL and tick *Unblock* — that applies to .NET plugins, not to
these; `LoadLibrary` never reads the mark. If the plugins do not appear, check
whether **Smart App Control** is on, which rejects unsigned DLLs silently.

Requires Resolume 7.3+; 7.4+ to see the Licence field's status in the panel
(on 7.3 it is in the licence folder's `README.txt` and Resolume's log). macOS
builds are universal (arm64 + x86_64), so Apple Silicon is native.

## Licence

Datamosh needs a licence key or a free trial from
[letissier.ie](https://letissier.ie). Without one, a new Datamosh passes video
through untouched and its last parameter reads **Licence: locked, ...**. Type
your key (`LT-DATA-...`), or your email address for a trial, into that
**Licence** field and press Enter; it unlocks within seconds, including effects
already in the composition. An effect that is already running is never locked,
and nothing waits on the network. Type `folder` for the licence folder and the
request code for offline activation.

Also typed into that field: `feedback` opens the feedback page, and `send`,
`discard`, `always send` and `reports off` answer or change the crash-report
setting.

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
- The macOS installer is signed and notarised. The **zip** still needs the
  quarantine flag cleared by hand — use the `.pkg` unless you have a reason not
  to. Windows binaries are unsigned, which matters only for Smart App Control.
- The mixer carries an inherited `mixVal` slider that does nothing — use its own
  **Mix** parameter. It cannot be renamed away without making the plugin
  unloadable; the reason is documented in the architecture page.
- Performance has only been measured on a software rasteriser. Run
  `datamosh_tests --profile` on real hardware before committing to 4K.

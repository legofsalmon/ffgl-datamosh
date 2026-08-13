# Validating a build in Resolume

Everything in this project is verified by 34 automated tests against synthetic
footage with known ground truth. None of it is verified against the actual host.
This document is how that gap gets closed.

## The one fact that shapes every check below

When the pipeline cannot run — a shader failed to compile, an allocation failed,
the frame gate is stuck, it is the first frame — `ProcessOpenGL` falls through to
`Passthrough`, which is literally:

```glsl
fragColor = texture( InputTexture, uv );
```

applied with the same `MaxUV` the working path uses. **A completely dead plugin
emits a pixel-exact copy of its input.** It looks like a correctly-bypassing
effect at every resolution, on every source, at every alpha.

So every check here is written to tell *working* apart from *inert*, never
*working* apart from *black*. Three rules follow, and they beat anything that
contradicts them:

1. **Never score a parameter by reading its value back in our own panel.**
   `GetParameterDisplay` returns our own stored float through our own getter. A
   round trip through our own vector cannot fail while the value is failing to
   reach the shader. If a parameter shows no visible effect at its extreme, the
   result is **UNKNOWN**, never PASS.
2. **Never score anything on an instance whose liveness you have not just
   confirmed.** See the canary below.
3. **Prefer hand-dragging a slider over picking a Style when proving liveness.**
   `Style → ApplyStyle → PushParam` never touches the host parameter path. A melt
   produced by picking "Melt" proves the shaders run; it does not prove Resolume
   can write a parameter.

**The liveness canary.** On the instance under test: Style = Custom, Motion
Threshold **0.0**, everything else default. Hand-drag **Mosh Amount 0 → 1**: the
picture must stop refreshing and smear within about a second. Drag it **back to
0**: the clean picture must return within one frame. Both moves go through the
host. If the canary does not hold, nothing else you observe on that instance
means anything.

**Getting back to a known state:** delete the effect and re-add it. A fresh
instance is default except that Motion Threshold starts at 0.15. Do not hand-dial
twenty-two controls.

**Viewing conditions:** judge visual steps on a full-resolution output or a
preview at 100%. Block Size, Softness, Quantise and Chroma Drift cannot be scored
on a small preview pane, and a false FAIL there costs an hour.

---

## Run the collector first

```sh
# macOS
bash tools/collect-datamosh-diagnostics.sh | tee ~/Desktop/datamosh-diag.txt

# Windows, from PowerShell
powershell -ExecutionPolicy Bypass -File .\tools\collect-datamosh-diagnostics.ps1 |
    Tee-Object $env:USERPROFILE\Desktop\datamosh-diag.txt
```

Both are strictly read-only — no `xattr -d`, no `Unblock-File`, no signing, HTTP
GET only — so they are safe to run at any point and they describe the machine as
it actually is. Every section is independent; a missing tool or folder prints
`SKIPPED` and the run continues. Add `--redact-user` / `-RedactUser` to replace
your username.

They answer, without you needing to know what matters: *are the files where
Resolume looks, are they loadable, did Resolume look, did Resolume load them,
did the parameters register.*

---

# The 10-minute smoke test

You need the two binaries, Resolume, and **one clip with strong continuous
motion** — a fast pan, water, smoke, or any Resolume generator that moves.
Nothing else. Don't prepare footage; don't open the log unless a step fails.

Gates: **A** registered · **B1** instantiates · **B2** renders and the host can
write parameters · **C** parameter ranges are real · **D1** mixer instantiates. A
gate failure invalidates everything after it — stop and record.

### S1. Install, and clear the download flag

Quit Resolume first. **Extract from a terminal, not from Finder or Explorer** —
Archive Utility propagates quarantine to every extracted member, which is what
makes a bundle root look clean while its contents are still quarantined.

**macOS**

```sh
ditto -x -k ~/Downloads/datamosh-*-macos-universal.zip ~/Downloads/datamosh-extract
DIR="$HOME/Documents/Resolume Arena/Extra Effects"      # edit this one line only
mkdir -p "$DIR"
cp -R ~/Downloads/datamosh-extract/*.bundle "$DIR"/
xattr -dr com.apple.quarantine "$DIR/Datamosh.bundle"
xattr -dr com.apple.quarantine "$DIR/DatamoshTransplant.bundle"
```

**Read the output of those `xattr` commands**, then verify:

```sh
find "$DIR"/*.bundle -exec xattr -p com.apple.quarantine {} \; 2>/dev/null | wc -l
lipo -archs "$DIR/Datamosh.bundle/Contents/MacOS/Datamosh"
nm -arch arm64 -gU "$DIR/Datamosh.bundle/Contents/MacOS/Datamosh" | grep -i plugmain
```

**PASS** — the count is `0`, `lipo` prints `x86_64 arm64`, `nm` prints a
`_plugMain` line. `com.apple.provenance` and `com.apple.macl` are harmless and
are **not** failures; only `com.apple.quarantine` blocks loading.

| FAIL | Means |
| --- | --- |
| `com.apple.quarantine` still present anywhere | Gatekeeper refuses the `dlopen`. Resolume shows nothing and logs nothing — indistinguishable from "never installed". |
| `xattr` printed `Operation not permitted` | Your terminal has no access to `~/Documents`. System Settings → Privacy & Security → Files and Folders. **The most common macOS stall, and silent unless you read the command's own output.** |
| `lipo` omits `arm64`, or no `plugMain` | A build problem, not an install problem. Stop. |

Do **not** run `codesign` to sign anything. The arm64 slices already carry a
linker ad-hoc signature (`flags=0x20002`), which is sufficient for `dlopen`; the
x86_64 slice is unsigned and that is also normal.

**Do not go looking for "Open Anyway".** On macOS 15+ that button lives in System
Settings → Privacy & Security and is the documented route for unsigned
*applications*. A plugin bundle `dlopen`ed by a host never triggers that dialog,
so there is no button to press. Removing the quarantine attribute is the only
route for an FFGL bundle.

**Windows** (PowerShell)

```powershell
$DIR = "$env:USERPROFILE\Documents\Resolume Arena\Extra Effects"   # edit this line only
New-Item -ItemType Directory -Force -Path $DIR | Out-Null
Expand-Archive ~\Downloads\datamosh-*-windows-x64.zip -DestinationPath ~\Downloads\datamosh-extract
Copy-Item ~\Downloads\datamosh-extract\*.dll $DIR
Get-ChildItem $DIR -Recurse | Unblock-File
Get-Item "$DIR\Datamosh.dll" -Stream * | Select-Object Stream
```

**PASS** — either result. This check is retained for tidiness, not as a gate:
`Zone.Identifier` does **not** stop Resolume loading a plugin. The mark is read
by `ShellExecuteEx`, not by `LoadLibrary`, so a native DLL loads whether or not
it carries one. The widely-repeated "unblock your plugin" advice comes from .NET
plugin hosts, whose assembly loader does consult it.

The Windows mechanism that genuinely does block these is **Smart App Control**,
which evaluates every PE image the loader touches regardless of origin and
rejects unsigned ones with no error — Resolume runs on and the plugins never
appear. If S2 fails on Windows 11, check `Windows Security → App & browser
control → Smart App Control` before anything else. Note it cannot be re-enabled
once turned off without reinstalling Windows.

### S2. The effect registered — **GATE A**

Launch Resolume, open the **Effects** panel, search "Datamosh".

**PASS** — an entry named **Datamosh** is listed. If the search finds nothing,
clear the field and scroll the whole list; third-party FFGL effects may appear
under their own group heading. Genuinely absent → **triage F1**, and stop.

*Note but do not gate on:* a plain grey placeholder instead of a magenta/cyan
block-displaced thumbnail. That means thumbnails weren't requested, or 160×120
was rejected. Minor. While the browser is open, scroll the effects list hard —
a crash here would mean the host allocated a thumbnail buffer smaller than the
160×120 we report.

### S3. The mixer instantiates — **GATE D1**

The mixer is not in the Effects panel. FFGL mixers appear in a layer's **Blend
Mode** dropdown, as **Mosh Transplant**.

**Do not score this by reading the list.** The dropdown is populated from the
prototype's `getInfo`, resolved in `plugMain` before any instance exists. The
historical phantom-parameter bug killed the plugin in `instantiateGL`, long after
the name reached the dropdown. A listed name proves nothing.

Put a clip on Layer 1 and a clip on Layer 2. Select Layer 2 → Video properties →
**Blend Mode = Mosh Transplant**.

**PASS, all three:** the blend mode stays selected; Layer 2 still renders
something other than black; the parameter panel opens with controls in it.

If the blend mode reverts on its own or the panel is empty, Resolume instantiated
and silently destroyed the instance — the exact historical failure. The mixer
phase is dead; the effect phases are unaffected.

If the entry reads **`Mosh Transplant`** in full, the name field is correct.
(`PluginInfoStruct::PluginName` is `char[16]` and is not null-terminated when
full; 15 characters is the real limit. A truncated name here means someone
lengthened it — there is a `static_assert` guarding this in `DatamoshMixer.cpp`.)

Set Layer 2 back to a normal blend mode before continuing.

### S4. The effect instantiates — **GATE B1**

Load the moving clip and play it. Drag **Datamosh** onto the **layer header** —
not onto the clip thumbnail. Dropping it on a clip attaches it to that clip only,
and it will vanish on the next clip launch and read as an intermittent failure.
Click the effect's name to open its parameters.

**PASS** — it stays in the chain and its panel opens with controls.

**FAIL** — nothing appears, or it appears and vanishes within a frame. `InitGL`
returned FF_FAIL, and there are only two causes: **a shader failed to compile on
this driver**, or the fullscreen quad could not be created. Render-target
allocation happens later and cannot make this step fail. Shader compilation is
the likely one: all 34 tests compile on Mesa llvmpipe, the most permissive GLSL
front end in existence, while Apple's GL 4.1 compiler is the strictest. At risk:
GLSL array constructors in `BlockMatch`, `isnan`/`isinf`, `textureLod` with a
computed level, `round()`. → **triage F1**.

Gate B1 passing means only that `InitGL` returned FF_SUCCESS. It says nothing
about buffers or rendering.

### S5. It renders, and the host can write a parameter — **GATE B2**

Motion Threshold → **0.0**. Then **hand-drag Mosh Amount 0 → 1.** Not a Style.
Watch for three seconds.

| What you see | Verdict |
| --- | --- |
| Refreshing stops within a frame and the picture smears along the motion; within 2–3 s it's an abstract dragged mess, covering the **whole frame**, with no shift or zoom versus bypass | **PASS** |
| It locks to one frozen frame and no control changes it | **FAIL — frame gate.** `SetTime` is being called with a constant. One frame advanced and none ever will. |
| The clip keeps playing perfectly cleanly at Mosh Amount 1.0 | **FAIL — never rendered, or parameter writes aren't arriving.** Disambiguate below. |
| It melts over only part of the frame, or the image shifts/zooms versus bypass | **FAIL — geometry.** `MaxUV` handling. |

**Disambiguating the "perfectly clean" case, 20 seconds:** set **Style = Melt**.
If it melts on Style but not on the hand-drag, the shaders and buffers are fine
and **Resolume is not delivering `SetFloatParameter`** — a distinct, nameable
failure, and every numeric observation later would be measuring defaults. If it
doesn't melt on Style either, either `InitGL` failed or `SetTime` first delivered
exactly `0.0`, matching `lastHostTime`'s initialiser, so `timeMoved` was false on
the first call, `hasHistory` never became true, and it renders pure passthrough
forever — indistinguishable from not being installed. → **triage F1**.

**Stop here on failure. Everything below is meaningless.**

### S6. The motion estimator is actually running

Still at Mosh Amount 1.0, Motion Threshold 0.0 — drag **Motion Gain to 0.0**.

**PASS** — moving regions **freeze dead in place**, holding old pixels at zero
displacement, while genuinely static regions keep refreshing.

This is the most unambiguous single observation available: it is binary, and it
separates "pixels are being held" from "motion is being estimated and applied".
If nothing changes at 0.0, the motion field is not reaching the mosh pass.

Put Motion Gain back to 1.0, then run the second half of the canary: Mosh Amount
back to 0, clean picture within one frame.

### S7. Are the parameter values real, or normalised? — **GATE C**

`CFFGLPluginManager::SetParamInfo` clamps `FF_TYPE_STANDARD` defaults to 0..1,
and `AddParam` patches the default back only when it lies *outside* 0..1. Every
out-of-range parameter we ship has a default inside 0..1 — Motion Gain 1.0 in
0..4, Burst Length 1.0 in 0.05..8, Motion Lag 0 in 0..15 — so the workaround
never fires and the question is never answered by construction.

Score it on two parameters on **two different FFGL type paths**, both by
behaviour. A displayed value is not evidence for either.

**(a) Burst Length — `FF_TYPE_STANDARD`.** Mosh Amount 0.0, Burst Length to
maximum (panel should read 8.00). Press **Trigger**. Time from the click until
the picture is clean again. You're distinguishing ~8 s from ~1 s, so a phone
clock is plenty. **PASS** ≈ 8 s plus a short ease-out. **FAIL** ≈ 1 s.

**(b) Motion Lag — `FF_TYPE_INTEGER`, a different host code path.** Mosh Amount
1.0, Motion Threshold 0.0, Motion Smoothing 0.0, Softness 0.0. Compare **Motion
Lag 1 against Motion Lag 15** back to back. **PASS** — grossly different
pictures. **FAIL** — they look the same.

Either failing = gate C fails: the host is sending normalised 0..1 where the
plugin expects the real range, so 8.0 arrives as 1.0 and 15 arrives as 1.
**Nothing looks broken — it just never gets as extreme as the labels promise.**
Settle this before any sweep, or every tuning verdict describes a compressed
range.

### S8. Is the shipping default usable? (60 seconds, high value)

Motion Threshold back to **0.15** (the shipping default), Mosh Amount 1.0,
ordinary footage.

At 0.15 the gate is 1.2 px/frame, and footage moving slower than that produces no
mosh at all no matter what Mosh Amount is. "Does nothing at the default on gentle
footage" is the exact symptom of a plugin that isn't loaded — which is why it's
worth settling in the first ten minutes. If a first-time user would see nothing,
**the default is wrong**; record the value at which it becomes clearly visible.

### Smoke-test scorecard

```
S1 quarantine / MotW cleared and verified   PASS / FAIL
   lipo -archs or PE machine                PASS / FAIL
   plugMain exported                        PASS / FAIL
S2 GATE A   effect in Effects panel         PASS / FAIL    thumbnail: colour / grey
S3 GATE D1  mixer instantiates              PASS / FAIL    name shown: ______
S4 GATE B1  effect stays, panel opens       PASS / FAIL
S5 GATE B2  hand-drag melts                 PASS / FAIL    Style-only melt? ____
S6 Motion Gain 0 freezes moving regions     PASS / FAIL
S7 GATE C   Burst 8.0 measured ____ s       PASS / FAIL
            Motion Lag 1 vs 15 differ       PASS / FAIL
S8 default 0.15 visible on real footage     YES  / NO      recommend: ______
```

If S2–S7 all pass, the plugins are real and doing something.

---

# Triage

## F1. Did Resolume look in your folder?

Search the log for `Scanning directory`. The collector does this for you and
prints the verdict.

| What you see | Means |
| --- | --- |
| No line naming your folder | Resolume never looked there. Wrong folder, or it needs adding under Preferences → Video. **This is the only place "never found" and "found and rejected" can be told apart.** |
| Scan line + `Library could not be loaded` / `Error: 193` | The OS refused the load: architecture mismatch, quarantine/MotW still set, or a missing VC++ runtime on Windows. Re-run S1 and the collector's arch probes. |
| Scan line + `plugin main function was unsuccessfull` | `plugMain` returned FF_FAIL. **The historical phantom-parameter class of bug** — a plugin bug, not an install problem. |
| Scan line, no error at all | Ambiguous. Check the collector's `plugMain` export probe — the other historical failure was a binary with no entry point, because `plugMain` lived in a translation unit nothing referenced and the linker discarded it. |
| Nothing at all, Resolume < 7.3.1 | The plugin log channel may not be picked up at all. Score UNAVAILABLE. |

**The easiest way to open the log is from inside Resolume: Preferences →
Feedback → View Log.** That avoids the path question entirely, and it is the
route Resolume's own FFGL documentation uses.

If you want the file itself: macOS `~/Library/Logs/Resolume Arena/`, Windows
`%LOCALAPPDATA%\Resolume Arena\`. Forum posts naming `%APPDATA%` (Roaming) are
outdated for v7 — the collector checks both anyway, since checking only one and
finding nothing is indistinguishable from there being no log. The filename is not
documented consistently, so open the newest file in the folder.

**Resolume's UI tells you nothing when a plugin fails to load** — no
notification, no badge, no dialog. "Never looked in that folder" and "looked,
loaded it, and rejected it" present identically: the effect is simply absent. The
log is the only thing that separates them, which is why this section exists.

## F2. The buffers line — anchored, so a stale line can't pass

Resolume's log is cumulative, and `datamosh: 1920x1080, 120x68 blocks, 59 MB of
buffers` is the same line every session. A line from yesterday's build, or from a
copy in the *other* candidate folder, satisfies a naive search. Anchor it:

1. Set the composition to **1366×768**. Note the log file's current size.
2. Delete and re-add the effect.
3. Search for `datamosh:` among lines newer than that mark.

**PASS** — exactly one new line reading `datamosh: 1366x768, 86x48 blocks, …`.
The block counts must be `86x48` (1366/16 = 85.4 → 86; 768/16 = 48; `BlocksAcross`
ceils). Numbers no previous session can have written.

**That proves exactly two things: every shader compiled, and every render target
allocated.** It does **not** mean anything rendered — `LogVramBudget()` runs from
`EnsureResources` before `PassIngest` draws. Rendering is proved by S5/S6 and by
nothing else. Score S5 *before* you read the log.

| Log line | Means |
| --- | --- |
| `failed to compile shader <name>` | Blocker, and it names the shader. Highest-probability failure on Apple's GL 4.1 compiler. |
| `could not allocate render targets` | `GL_RGBA16F` / `GL_R16F` / `GL_RGBA32F` not colour-renderable at this size, or VRAM exhausted. **Note the shape:** the effect sits in the chain with every parameter movable, renders passthrough, and re-attempts the whole teardown-and-reallocate *every frame*. The layer degrades gracefully; the application grinds. |
| The buffers line **repeats**, near once per frame | The instance is being asked for two sizes alternately — output and preview. `EnsureResources` reallocates and resets `hasHistory` on every size change, so accumulation can never build. Presents as a faint stutter rather than an effect, and the repeating line is the only way to see it. |

If you can't see the log at all, run Resolume from a terminal so the SDK's
shader-error `printf` reaches a console:

```sh
"/Applications/Resolume Arena/Resolume Arena.app/Contents/MacOS/Resolume Arena"
```

---

# The full pass

Only worth starting once the smoke test is green. Roughly four hours end to end;
each phase is independently useful, so stop wherever you run out of time.

### What needs which Resolume version

From FFGL.h's own changelog, so a missing feature can be told apart from a bug:

| Feature | Needs |
| --- | --- |
| Parameter groups (`SetParamGroup`) — collapsible sections | 7.3.0 |
| Plugin logging into the host log (our `datamosh:` lines) | 7.3.1 |
| Parameter display names | 7.4.0 |
| REST API and WebSocket API | 7.8 |
| Native Apple Silicon | 7.11 |
| Bundled MCP servers; monitor snapshot endpoints | 7.26 |

On anything below 7.3, parameters still work but appear as one flat list — that
is the host, not us. Below 7.3.1 there will be no `datamosh:` lines in the log no
matter what happens, so score that channel UNAVAILABLE rather than FAILED.

### The phases

**Phase A — the parameter contract (~30 min).** A failure here means every number
you read later isn't the number the plugin received, so run it before any sweep.
Select every entry of every dropdown including both endpoints. Verify Block Size
visibly changes tile size across its 8× range. Verify the Style guard: apply a
style, touch one parameter it owns, confirm the dropdown reverts to Custom —
then confirm pressing Trigger does *not* revert it.

**Phase B — signal correctness (~20 min).** With Mosh Amount at 0 the output must
be pixel-identical to bypass, in both geometry and alpha. Any shift, zoom or edge
difference is `MaxUV` or premultiplication.

**Phase C — cut detection and GL hygiene (~35 min).** Put other effects before
and after ours in the chain and confirm neither is disturbed — we must not leave
blend state, bound FBO, viewport or active texture unit dirty. Then Auto Mode →
On Cut against footage with real cuts.

**Phase D — the mixer (~20 min).** Inputs of different sizes; what Resolume does
with the inherited `mixVal` slider; whether the Motion Source labels tell the
truth.

**Phase E — aesthetic sweeps (~55 min).** Every parameter, one at a time, with
the preconditions that make its effect visible. This is where the tuning defaults
get their first contact with real video — record a recommended value for anything
whose default reads wrong.

**Phase G — performance and resources (~35 min).** VRAM per instance; whether
Style and Block Size changes reallocate; resolution churn; a 30-minute soak.

**Phase R — the restart batch (~15 min).** Everything requiring a restart, run
once rather than scattered through the day.

---

# What to report back

Send these four together and almost any failure here can be diagnosed in one
round trip.

1. **The collector output**, whole and unedited. Redact with `--redact-user` if
   you like; don't trim sections.
2. **A log excerpt** from launch to failure. Note the log's byte size *before*
   the action so the excerpt can be bounded to what's new; otherwise send the
   last 500 lines and say so.
3. **The scorecard above**, filled in — including what you did *not* test.
4. **A screen recording, or two stills at the same frame**, for any visual FAIL.
   For a before/after comparison, two stills beat any prose description.

Plus the basics: Arena or Avenue and its version, OS and architecture, GPU and
driver, composition resolution, and — importantly — **how many Extra Effects
folders held a Datamosh binary at test time**. If it's more than one, you can't
tell which was loaded; empty all but one and start again.

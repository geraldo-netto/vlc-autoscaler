# AutoUpscale — Command Line Recipes

This document collects working VLC command lines for the autoupscale
filter, with the **trade-offs** and **failure modes** of each.

If a recipe doesn't work for your video, the [Diagnostic ladder](#diagnostic-ladder)
at the bottom walks you through narrowing down the cause.

## Quick table — which recipe should I use?

| Situation | Recipe |
|---|---|
| **You want a sane default that just works** | [**Recipe 9: Quiet, stable upscaling**](#recipe-9-quiet-stable-upscaling-recommended-for-everyday-use) |
| First time, just want it to work | [Recipe 1: Minimal](#recipe-1-minimal) |
| You ran the recipe from the README and got recursion errors | [Recipe 2: Transcode bypass](#recipe-2-transcode-pipeline-bypassing-the-recursion-limit) |
| You see occasional black screens | [Recipe 4: Without postproc](#recipe-4-without-postproc-recommended-default) |
| Hardware-decoded source (VAAPI/VDPAU/4K source) | [Recipe 5: HW-decode safe](#recipe-5-hw-decode-safe) |
| Maximum quality, you have CPU to spare | [Recipe 6: Quality-first](#recipe-6-quality-first) |
| Low-latency live stream | [Recipe 7: Low-latency](#recipe-7-low-latency) |
| You want to encode to a file, not display | [Recipe 8: Save to file](#recipe-8-save-to-file) |
| Log is full of `pulse audio output warning` and dropped frames | [Reducing log noise](#reducing-log-noise-pulseaudio-clock-errors-and-frame-drops) |
| Diagnosing a specific bug | [Diagnostic ladder](#diagnostic-ladder) |

---

## Recipe 1: Minimal

The simplest possible invocation. The filter sits in VLC's normal video
filter chain and AUTO-detects target resolution.

```sh
vlc --video-filter=autoupscale path/to/video.mp4
```

**What it does:**
- Source is opened normally (HW decode if available, falls back to SW)
- AutoUpscale picks the target: nothing if source ≥ 720p, otherwise 720p
  or 1080p depending on source size and the 4× ratio cap
- Output goes to the screen

**When it works:**
- Sub-720p source (the filter's design point)
- Source chroma is I420, YV12, I422, I444, or NV12 (VLC will auto-convert
  hardware chromas to I420)

**When it breaks:**
- Hardware-decoded sources may hit `Too high level of recursion (3)` because
  VLC inserts a converter, then postproc, then autoupscale. See [Recipe 2](#recipe-2-transcode-pipeline-bypassing-the-recursion-limit).
- 4K-or-higher source: AutoUpscale bypasses (won't downscale), so
  filter does nothing.

**Why this isn't the default in the README:**
This is the canonical form. The README's example uses `--sout` because it
also avoids VLC's filter-chain depth limit at high resolutions, but for
typical 480p→1080p use this is fine.

---

## Recipe 2: Transcode pipeline (bypassing the recursion limit)

This is the recipe that survived all our testing. It runs the filter
inside the transcode stage, which has its own filter chain separate from
the display's chain — so VLC's `MAX_CHAIN_LEVEL=3` doesn't bite.

```sh
vlc --autoupscale-threads=16 \
    --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,
                       venc=x264{preset=ultrafast,tune=zerolatency},
                       vfilter=autoupscale}:display' \
    path/to/video.mp4
```

**What it does:**
- Transcodes the video stream through autoupscale → x264
- Encoded H.264 + AAC are then displayed
- Bypasses the `MAX_CHAIN_LEVEL=3` limit that affects the direct display path

**When it works:**
- Most sub-720p content from MP4/MKV containers
- Hardware-decoded sources (the recursion limit bites them but transcode is upstream)
- Mixed-codec sources (audio re-encoded to AAC alongside video to H.264)

**When it breaks:**
- **`postproc filter warning: Quantification table was not set by video decoder.`**
  appears on most ffmpeg-decoded sources. With frame-threaded H.264 (default),
  postproc receives stale or missing QP data and may produce inconsistent
  output, including occasional black or near-black frames during scene changes.
  → Drop `postproc:` (see [Recipe 4](#recipe-4-without-postproc-recommended-default)).
- **`picture is too late to be displayed (missing N ms)`** in the log means
  the pipeline can't keep up. At 50 fps source upscaled to 1080p, your
  budget is 20 ms/frame. If the upscale + USM exceeds that, frames drop;
  in worst cases the vout shows the previous buffer (often black).
  → Use [Recipe 7](#recipe-7-low-latency) or lower target resolution.
- **CPU usage is high.** This recipe re-encodes to H.264, which costs ~5-8 ms
  per frame on top of the upscale. If you don't need a re-encoded stream
  (e.g. you're not network-streaming this), use [Recipe 1](#recipe-1-minimal) instead.

**Tunable parameters:**
- `--autoupscale-threads=N` — workers for the upscaler. Default: cores/2 − 2 (so on a 32-core box you get 14 workers, leaving 18 cores for VLC, decoder, encoder, audio, and other libraries). Set lower if you want headroom for the rest of the system, or higher if you measured the upscaler being CPU-starved.
- `vb=10000` — video bitrate in kbps. Higher = better quality, more CPU.
- `preset=ultrafast` — x264 speed/quality tradeoff. `ultrafast` is fastest
  but lowest quality; `medium` is balanced; `slow` is high-quality but may
  drop frames in real time.

---

## Recipe 3: Pure display, target=1080p

Skip transcode entirely. Rely on VLC's normal display chain. Works for
most software-decoded sources.

```sh
vlc --video-filter=autoupscale \
    --autoupscale-target=2 \
    --autoupscale-threads=16 \
    path/to/video.mp4
```

**What `--autoupscale-target=2` means:**
- 0 = AUTO (default; never picks above 1080p; uses ratio cap)
- 1 = 720p
- 2 = 1080p
- 3 = 1440p
- 4 = 4K (3840×2160)
- 5 = 5K (5120×2880)
- 6 = 8K (7680×4320)

**When it works:**
- Software-decoded H.264/HEVC of any sub-target source
- 480p → 1080p, 540p → 1080p, 720p → 1440p, etc.
- One-pass real-time playback without re-encoding

**When it breaks:**
- Hardware-decoded sources at high target resolutions: chain depth limit
  may fire. → Use [Recipe 5](#recipe-5-hw-decode-safe).
- Display refresh > target fps causes some judder; not autoupscale's fault
  but worth noting.

**Trade-off vs Recipe 2:**
- This recipe is **simpler and lower-CPU** (no re-encode).
- This recipe **fails on more configurations** because it relies on VLC's
  display chain rather than transcode chain.

---

## Recipe 4: Without postproc (RECOMMENDED DEFAULT)

```sh
vlc --autoupscale-threads=16 \
    --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,
                       venc=x264{preset=ultrafast,tune=zerolatency},
                       vfilter=autoupscale}:display' \
    path/to/video.mp4
```

(Same as [Recipe 2](#recipe-2-transcode-pipeline-bypassing-the-recursion-limit)
but with **no `postproc:` prefix** in `vfilter=`.)

**Why this is recommended over Recipe 2:**
- `postproc` was originally meant for severely compressed legacy content
  (DVD-era MPEG-2). On modern H.264/HEVC sources, it depends on
  quantization tables that ffmpeg's threaded decoder doesn't always provide
  in time. The result: a startup warning, and occasional output glitches.
- AutoUpscale already produces a high-quality result via Spline36 + USM.
  Running postproc first **fights** the upscaler — it deblocks edges that
  the upscaler then has to reconstruct anyway.
- Removing postproc usually **improves** perceived quality and removes
  one source of intermittent black/garbage frames.

**When you'd still want postproc:**
- Source is genuinely low-quality MPEG-2 with visible blocking
- Source is sub-DVD bitrate H.264 with heavy macroblocking
- You've A/B-tested and verified postproc helps your specific content

---

## Recipe 5: HW-decode safe

For sources where VLC chooses VAAPI, VDPAU, or another hardware decoder
and you hit `Too high level of recursion (3)`.

```sh
vlc --avcodec-hw=none \
    --autoupscale-threads=16 \
    --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,
                       venc=x264{preset=ultrafast,tune=zerolatency},
                       vfilter=autoupscale}:display' \
    path/to/video.mp4
```

**What `--avcodec-hw=none` does:**
- Forces VLC to use software decode regardless of available HW acceleration
- Avoids VLC's hardware→software converter (one less filter in the chain)
- Eliminates the chain-depth recursion that was caused by inserting that
  converter ahead of postproc and autoupscale

**Cost:**
- Software H.264 decode at 1080p uses ~10-15% of one core
- For 4K+ sources, software decode can be CPU-bound; use a smaller target
- AutoUpscale's own multi-threading is unaffected

**Alternative: patch VLC.** The repo ships
`patches/vlc-3.0-raise-max-chain-level.patch` which raises `MAX_CHAIN_LEVEL`
from 3 to 5 in VLC's source. If you build VLC yourself this is a permanent
fix; if you use distro packages, the `--avcodec-hw=none` workaround is
your only option.

---

## Recipe 6: Quality-first

When you have CPU to spare and want the best output.

```sh
vlc --autoupscale-threads=24 \
    --autoupscale-target=2 \
    --autoupscale-algo=3 \
    --autoupscale-usm=50 \
    --autoupscale-content-probe=1 \
    --autoupscale-zerocopy-dst=0 \
    path/to/video.mp4
```

**What changed vs Recipe 1:**
- `--autoupscale-algo=3` — Spline36 (the highest-quality resampler).
  Default is already 3, this just makes it explicit.
  - 0 = Bilinear (fastest, blurriest)
  - 1 = Bicubic
  - 2 = Lanczos
  - 3 = Spline36 (recommended for upscaling)
- `--autoupscale-usm=50` — stronger sharpening (default 20). Range 0-200.
  At 50, edges are visibly crisper; at 100 it starts to look "digital";
  at 200 it produces ringing.
- `--autoupscale-content-probe=1` — diagnostic logging on. The probe
  measures source detail and warns if upscaling a heavily-compressed
  source is making it worse.
- `--autoupscale-zerocopy-dst=0` — workers write to scratch buffers,
  then copy out to VLC's destination picture. Slightly slower than
  the default zero-copy path but eliminates a class of edge-case bugs
  (see Recipe 9).

**Cost:**
- USM at 50% costs ~2.5× the default 20%
- Spline36 is already default, so no change there
- The copy-out path adds ~1-2 ms per frame at 1080p

**When NOT to use this:**
- Real-time playback of 60 fps source: budget too tight
- 4K target: USM time scales with output size, not source

---

## Recipe 7: Low-latency

For live streams, RTSP, or very latency-sensitive playback.

```sh
vlc --file-caching=300 \
    --network-caching=300 \
    --clock-jitter=0 \
    --clock-synchro=0 \
    --autoupscale-threads=8 \
    --autoupscale-usm=0 \
    --video-filter=autoupscale \
    rtsp://your.stream/here
```

**What each option does:**
- `--file-caching=300` and `--network-caching=300` — cap input buffering
  at 300 ms (default is 1000+ ms; that's where most "VLC is slow" comes from)
- `--clock-jitter=0 --clock-synchro=0` — disable VLC's clock-drift correction.
  Better for live streams where the source clock IS the truth.
- `--autoupscale-threads=8` — fewer workers means lower per-frame overhead
  from thread dispatch (the synchronization is small but non-zero)
- `--autoupscale-usm=0` — disables sharpening entirely. USM takes a fast
  identity path when amount=0 — no thread spawn, no workspace allocation.

**Cost:**
- Lower buffering means more sensitive to network jitter; if your source
  has bursty delivery, you'll get more dropped frames
- Disabling sharpening loses image quality

**Trade-off:**
- This recipe trades **about 20% of perceptual quality** for **~700 ms
  less playback latency**. Worth it for live; not for movies.

---

## Recipe 8: Save to file

Encode the upscaled video to a file instead of displaying.

```sh
vlc -I dummy --no-audio \
    --autoupscale-threads=24 \
    --autoupscale-target=2 \
    --sout='#transcode{vcodec=h264,vb=12000,
                       venc=x264{preset=medium,crf=18},
                       vfilter=autoupscale}:standard{access=file,mux=mp4,dst=output.mp4}' \
    --play-and-exit \
    path/to/input.mp4
```

**What changed:**
- `-I dummy` — no GUI, runs in headless mode
- `--no-audio` — skip audio entirely (add it back with the audio transcode
  options from Recipe 2 if you want sound)
- `preset=medium crf=18` — slower but much higher quality x264 settings
  than ultrafast (which is for real-time). `crf=18` is roughly visually
  lossless; `crf=23` is a smaller file at default quality.
- `standard{access=file,mux=mp4,dst=output.mp4}` — write to disk
- `--play-and-exit` — quit when done

**When it works:**
- Any input file that VLC can play
- Encoding speed is determined by `preset=`. `medium` runs at ~30-50 fps
  on a 16-core machine for 1080p output.

**Verifying output:**
```sh
# Check resolution
ffprobe -v error -select_streams v:0 \
    -show_entries stream=width,height,r_frame_rate output.mp4

# Compute MD5 of decoded frames (compare across runs)
ffmpeg -hide_banner -loglevel error -i output.mp4 -f md5 -
```

---

## Diagnostic ladder

When something's broken — black screens, freezes, garbage output, no upscale —
run these recipes **in order** and note where the symptom changes. That
isolates which component is at fault.

### Step 1: Bypass autoupscale entirely

```sh
vlc --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,
                       venc=x264{preset=ultrafast,tune=zerolatency}}:display' \
    path/to/video.mp4
```

- **If symptom persists** → autoupscale is innocent; the bug is in
  VLC's decoder, transcoder, or vout. File a bug with VLC, not us.
- **If symptom goes away** → autoupscale or its filter chain is involved.
  Continue to step 2.

### Step 2: Add autoupscale, no postproc

```sh
vlc --autoupscale-threads=16 \
    --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,
                       venc=x264{preset=ultrafast,tune=zerolatency},
                       vfilter=autoupscale}:display' \
    path/to/video.mp4
```

- **If symptom appears** → bug is in autoupscale itself. Continue to step 3.
- **If symptom is gone** → bug was caused by postproc (the most common
  culprit, see [Recipe 4](#recipe-4-without-postproc-recommended-default)).
  You're done.

### Step 3: Disable zero-copy

```sh
vlc --autoupscale-threads=16 \
    --autoupscale-zerocopy-dst=0 \
    --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,
                       venc=x264{preset=ultrafast,tune=zerolatency},
                       vfilter=autoupscale}:display' \
    path/to/video.mp4
```

- **If symptom appears** → bug is in autoupscale's core path (not
  zerocopy-specific). Continue to step 4.
- **If symptom is gone** → bug is in autoupscale's zero-copy destination
  path. Workaround: set `--autoupscale-zerocopy-dst=0` always. File a
  bug report with the verbose log; we'd want to investigate.

### Step 4: Single thread, lowest target

```sh
vlc --autoupscale-threads=1 \
    --autoupscale-target=1 \
    --autoupscale-zerocopy-dst=0 \
    --autoupscale-usm=0 \
    --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,
                       venc=x264{preset=ultrafast,tune=zerolatency},
                       vfilter=autoupscale}:display' \
    path/to/video.mp4
```

- **If symptom appears** → bug is in the deterministic core (no threading,
  no USM, simplest target). Capture full `--verbose=2` output and file a
  bug report. This is rare — the unit tests + concurrency stress tests
  cover this path heavily.
- **If symptom is gone** → narrow further: enable threads OR USM OR higher
  target one at a time to isolate which dimension triggers it.

### Step 5: Capture diagnostic info

If you've reached this step, please include in any bug report:

```sh
vlc --verbose=2 [your-failing-command] 2>&1 | tee /tmp/autoupscale.log
grep -iE "autoupscale|filter|chroma|recursion|error|fail|warning" /tmp/autoupscale.log
```

We want to see:
- Source chroma fourcc (look for `source chroma:` lines)
- The `AutoUpscale engaged: ...` line confirming dimensions and backend
- Any `filter chain:` messages
- Any errors or warnings
- Whether `postproc filter warning: Quantification table` appears

---

## Audio: starting at full volume

VLC 3.x doesn't have a `--volume=N` option (it was deprecated in 2.1).
Volume is controlled by the audio output plugin (PulseAudio in your case)
plus VLC's saved-volume-on-exit behavior. Three ways to fix audio
starting at 0%:

### Method 1 (recommended): Edit `~/.config/vlc/vlcrc`

Find or add these lines:

```ini
# Don't save the GUI volume when VLC exits — start at the saved baseline
# every time
qt-autosave-volume=0
```

Restart VLC, set the volume to 100% manually once via the slider, exit.
The next session will start at 100%. (With `qt-autosave-volume=0`, VLC
won't lower the saved baseline when you exit at 0%, so this only matters
for one round trip.)

### Method 2: Per-app volume in PulseAudio

PulseAudio remembers per-app volume separately from VLC's slider. If
PulseAudio remembers VLC at 0%, no VLC option fixes it — you have to
reset it at the audio server level:

```sh
# Find VLC's stream in PulseAudio
pactl list sink-inputs | grep -B2 -A20 vlc

# Reset its volume to 100% (use the index from above)
pactl set-sink-input-volume <index> 100%

# Or set the per-app default for VLC permanently:
pactl set-sink-input-mute <index> 0
```

### Method 3: `--gain` (workaround, not a real volume control)

`--gain=N` adds compressor pre-amp. Range 0.0-8.0; 1.0 is unity.

```sh
vlc --gain=1.0 [other options] path/to/video.mp4
```

**Caveats:**
- This is the COMPRESSOR pre-amp, not the volume slider. If the slider is
  at 0%, gain doesn't help (0 × anything = 0).
- Gain values > 2.0 cause clipping/distortion on most content.
- Most useful for boosting genuinely quiet audio, not for fixing a 0%
  startup volume.

### Why your audio "starts at 0"

Your log shows VLC connecting to a Bluetooth sink:

```
pulse audio output debug: changing sink 6947: bluez_output.F4_4E_FD_01_53_0F.1 (BW01)
```

Bluetooth devices in PulseAudio often have their own remembered volume
that's separate from VLC's slider. The combination "VLC slider at 100%"
+ "Bluetooth sink remembered at 0% from last connection" produces silence.
Use **Method 2** (`pactl set-sink-input-volume`) — it's the only level
that lets you set the per-device volume directly.

---

## Reducing log noise: PulseAudio clock errors and frame drops

If you see message floods like these in your VLC log:

```
pulse audio output warning: starting late (-111651650002536464 us)
main audio output warning: playback way too late (...): flushing buffers
vlcpulse audio output debug: write index corrupt
pulse audio output debug: cannot synchronize start
pulse audio output debug: deferring start (1365156 us)
pulse audio output debug: underflow
main audio output warning: playback way too early (-1160971): playing silence
avcodec decoder warning: More than 11 late frames, dropping frame
```

…the negative trillion-microsecond values (~3540 years) are not real
timing measurements — they're symptoms of a **broken audio clock**.
VLC computes "lateness = expected − now" and gets nonsense, retries,
underflows, recovers, and the video decoder drops frames trying to
keep up with the wandering audio clock.

This is a VLC / PulseAudio / source-file interaction; it's **not**
caused by autoupscale. The plugin doesn't touch audio or timestamps.
But there are several flags that quiet the noise and stabilize playback.

> **Note on master-clock control:** VLC 4.x adds a runtime
> `--clock-master=` option that lets you make video the canonical
> clock (audio resamples to follow). VLC 3.x — including 3.0.20 — does
> **not** expose this; the master is fixed to audio when audio is
> present. If you're on VLC 4.x you can use it; on 3.x the only
> levers are the ones below.

### Approach 1: Switch audio output to ALSA (bypass PulseAudio)

Most of the `pulse audio output` and `vlcpulse` messages come from
PulseAudio's clock-sync layer. Going direct to ALSA skips that entire
stack:

```sh
vlc --aout=alsa ...
```

Or persistently in `~/.config/vlc/vlcrc`: `aout=alsa`.

Trade-off: you lose PulseAudio's per-app routing and Bluetooth
support, but you also stop having PulseAudio's bugs reported to you.
ALSA's clock is taken directly from the sound card, which doesn't
suffer the same desync problems.

### Approach 2: Increase caching to absorb clock jitter

```sh
vlc --file-caching=2000 --network-caching=2000 ...
```

Default file caching is ~300 ms; bumping it to 2 seconds gives the
audio pipeline more headroom before declaring underflow. Doesn't
fix the root cause but reduces the rate of complaints.

### Approach 3: Enable audio time-stretching

```sh
vlc --audio-time-stretch ...
```

When the audio clock drifts, VLC speeds up or slows down audio
playback (without changing pitch) to stay aligned with video. This
is enabled by default in some VLC builds and disabled in others —
explicitly turning it on prevents the frame-drop cascade in many
cases. Negligible CPU cost.

### Approach 4: Compensate fixed audio desync

If your source file has a constant audio offset, give VLC the
correction directly (in milliseconds):

```sh
vlc --audio-desync=50 ...    # play audio 50 ms LATER than video
vlc --audio-desync=-100 ...  # play audio 100 ms EARLIER than video
```

This doesn't help with random clock jitter, but it does help if your
log shows a **consistent** "way too late by N seconds" value across
playbacks of the same file.

### Approach 5: Lower verbosity (suppress messages without fixing them)

Most of those messages are at warning or debug level. They show up
when you run with `--verbose=1` or `--verbose=2`:

```sh
vlc ...                # no --verbose flag: only errors shown
vlc --verbose=0 ...    # explicit
vlc --quiet ...        # equivalent to --verbose=0
```

If you didn't pass `--verbose=2` but still see them, check
`~/.config/vlc/vlcrc` for a `verbose=2` line.

### Approach 6: Disable audio entirely (if you don't need it)

```sh
vlc --no-audio ...
```

Kills the audio pipeline — no audio clock to be broken, no decoder
racing to keep up with it. Frame dropping stops because video uses
its own clock. This is what every test command in this codebase
uses; it's the most certain way to eliminate the entire class of
warning.

### Approach 7: Confirm autoupscale isn't contributing

Quick sanity check that the filter isn't the cause:

```sh
vlc --no-video-filter your_video.mp4
```

If frame drops persist without the filter, the cause is upstream
(decoder, demuxer, audio pipeline). If they vanish without the filter
and reappear with it, you may be:

- Targeting too high a resolution for your CPU at the source's
  framerate. Lower `--autoupscale-target` (try `=2` for 1080p or
  `=1` for 720p).
- Running the SSE2 fallback variant. Check the engagement log — it
  should show `simd=avx512` or `simd=avx2` on a modern CPU; if it
  shows `simd=sse2` you're missing the SIMD speedup. Rebuild without
  `MULTIVERSION=0`, or with the right `MARCH=` baseline.

---

## Recipe 9: Quiet, stable upscaling (recommended for everyday use)

A starting-point invocation that combines autoupscale's tested defaults
with the noise mitigations from above. This is verified to start cleanly
in VLC 3.0.20.

```sh
vlc \
  --aout=alsa \
  --audio-time-stretch \
  --file-caching=2000 \
  --network-caching=2000 \
  --no-stats \
  --verbose=0 \
  --autoupscale-target=0 \
  --autoupscale-threads=0 \
  --autoupscale-content-probe=1 \
  --sout='#transcode{vcodec=h264,vb=10000,venc=x264{preset=ultrafast},vfilter=autoupscale}:display' \
  /path/to/your/video.mp4
```

**What each flag does:**

| Flag | Why |
|---|---|
| `--aout=alsa` | Bypass PulseAudio's clock-sync layer. Eliminates `vlcpulse` warnings entirely. Drop this if you need Bluetooth or per-app PulseAudio routing — leave it in if you're tired of the warnings and have ALSA configured. |
| `--audio-time-stretch` | Audio adjusts to track the input PTS instead of forcing video to chase a wandering audio clock. Reduces the frame-drop cascade when timestamps drift. |
| `--file-caching=2000` | 2-second buffer (default ~300 ms) absorbs short clock jitter without underflowing. |
| `--network-caching=2000` | Same buffer for network sources. |
| `--no-stats` | Suppresses end-of-playback statistics dump. |
| `--verbose=0` | Only show actual errors; hide warnings and debug. |
| `--autoupscale-target=0` | AUTO mode — picks 720p or 1080p based on source. Never goes above 1080p in AUTO. |
| `--autoupscale-threads=0` | Auto: `cores/2 − 2` workers. On a 32-core box that's 14 — leaves 18 cores for decoder, encoder, audio, OS. |
| `--autoupscale-content-probe=1` | Diagnostic only — measures source content quality and logs an advisory if upscaling looks unhelpful. ~27 µs/frame for 60 frames at startup, then off. |
| `#transcode{...}:display` | Re-encode in a single pipeline, then display. Avoids the recursion mode that `vfilter=` directly into display sometimes hits. |
| `vcodec=h264,vb=10000,venc=x264{preset=ultrafast}` | x264 ultrafast preset — costs ~5-8 ms/frame, well under the 16.7 ms budget for 60 fps. |

**For file output instead of display**, replace `:display` with:

```
:standard{access=file,mux=mp4,dst=/path/to/output.mp4}
```

**For target other than AUTO**, replace `--autoupscale-target=0` with:

| Value | Meaning |
|---|---|
| `1` | force 720p |
| `2` | force 1080p |
| `3` | force 1440p |
| `4` | force 4K |
| `5` | force 5K |
| `6` | force 8K |

Note that targets above 1080p require source ≥ 1/4 the target height
(the ratio cap) and significant CPU; on a 32-core machine 4K is
usually sustainable, but verify with the engagement log and frame-drop
count at the end of playback.

**If you need the chain-recursion fix**, apply
`patches/vlc-3.0-raise-max-chain-level.patch` to your VLC source and
rebuild — that's a compile-time patch that raises an internal
`MAX_CHAIN_LEVEL` constant, not a runtime flag. See [Recipe 2](#recipe-2-transcode-pipeline-bypassing-the-recursion-limit)
for context on when this is needed.

**Verifying it's working:** the engagement log line shows up at
`--verbose=2` (above we suppressed it). To confirm autoupscale ran:

```sh
vlc --verbose=2 [rest of args] 2>&1 | grep "AutoUpscale engaged"
```

Expected output:

```
AutoUpscale engaged: 854x480 -> 1280x720 (backend=zimg preset=0 algo=3 \
  usm=20 fps_target=60 threads=14 cores=32 mem=...MB simd=avx512)
```

The `simd=avx512` confirms the runtime dispatcher picked the AVX-512
variant. On Zen 1-3 you'll see `simd=avx2`; on pre-Haswell hardware
`simd=sse2`.

---

## See also

- [README.md](../README.md) — install, build, and quick start
- [docs/HOW_IT_WORKS.md](HOW_IT_WORKS.md) — internal design, threading
  model, perfmon, content probe
- [patches/](../patches/) — optional VLC source patches

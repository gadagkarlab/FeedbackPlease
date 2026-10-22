# FeedbackPlease

Real-time birdsong syllable detection and feedback in C++, ported from the LabVIEW
`DetectSyllables` / `CalcDetectSignal` / feedback-state-machine VIs.

It reads one microphone channel through NI-DAQmx (40 kHz, 40-sample frames = 1 ms updates),
computes a band-limited amplitude envelope over the last 15 ms, segments syllables, and classifies
each one against your list of syllable definitions (IOI / SD windows). The matched syllable's code goes
out on a digital port, and when it is the **target** syllable, feedback is delivered: a waveform on an AO
channel plus TTL lines. The GUI shows the raw microphone voltage and the detection signal live, and a
Tune tab lets you define and tune syllables on a folder of recordings.

## Build

Requirements: Visual Studio 2022/2026 with C++ (CMake is included), NI-DAQmx (C API is found at
`C:\Program Files (x86)\National Instruments\Shared\ExternalCompilerSupport\C`), and an internet
connection for the first configure (it downloads GLFW 3.4, Dear ImGui 1.90.9 and ImPlot 0.16).

```bash
cmake -S . -B build
cmake --build build --config Release
```

Outputs in `build/Release/`:

| Executable | Purpose |
|---|---|
| `FeedbackPlease.exe` | The app. `FeedbackPlease.exe [settings.ini] [--autostart]` |
| `offline_detect.exe` | Runs the detector over a WAV file as fast as possible, writes events + envelope CSVs. Use it to tune thresholds on recorded song. |
| `fp_tests.exe` | Unit tests for the DSP, segmentation, sequence matching and state machine. |
| `fp_engine_test.exe` | Runs the real-time engine headless on simulated song for 4 s and lists the NI devices it can see. |
| `make_test_wavs.exe` | Writes a folder of synthetic recordings (song bouts, calls, cage noise) for trying the Tune tab. |

Build without hardware support with `-DFP_WITH_DAQMX=OFF`, or without the GUI with `-DFP_BUILD_GUI=OFF`.

## Running

1. Start with **Source = Simulated** and press **Start**. With no WAV file chosen it plays a built-in
   synthetic song whose 4th syllable matches the default "target" syllable (`IOI, SD, IOI`), so you should see
   matches and feedback lines every ~1.2 s. Point **Song WAV** at a real recording to tune thresholds.
2. Open **Hardware / Source**, set the device names (the panel lists the devices it found), and switch
   **Source** to NI-DAQmx. The defaults say `Dev1`; this PC has `Dev2`, `Dev3` (PCIe-6321) and `PXI1Slot6` (PXIe-6341).
3. Settings are saved to `feedbackplease.ini` on exit (or with **Save settings**). Each session also saves
   a copy of the settings next to its recordings in `recordings/`.

Detection settings (bands, thresholds, syllables, target, feedback probabilities/delays) apply live while
running. Hardware, wave files and recording options apply at the next Start.

## Recording

Each Start creates `recordings/session_<date>_<time>_*` files (folder set under **Recording**):

- `_settings.ini`: the settings used.
- `_events.csv`: every onset, offset, syllable classification, match, feedback, song on/off, with the
  sample index since Start.
- **Song-triggered WAVs** `_song001.wav`, `_song002.wav`, ...: when the song detector turns on, a new
  file starts with the last **Pre-roll** seconds of audio (kept in a ring buffer, so the start of the song
  is included) and continues until song has been off for **Post-roll** seconds. Song resuming within the
  post-roll extends the same file. A song that starts soon after a file closed still gets its full
  pre-roll (it may overlap the previous file).
  - Channels, each optional, in this order: **raw mic** (V), **detection signal** (linear, held per
    1 ms frame) and **marker track** = 0.1 song + 0.2 syllable + 0.3 target match + 0.4 feedback (the TTL
    line states; every combination is a distinct level).
  - `_songs.csv` lists each file with its start / end sample on the same clock as the events CSV, its
    duration and the pre-roll it actually got (less than requested right after Start).
- `_envelope.wav` (optional): the continuous detection signal at the frame rate.

All writing happens on a background thread; the acquisition thread only queues data.

## Tuning on recordings (Kernel Extraction equivalent)

The **Tune on recordings** tab replaces LabVIEW's Kernel Extraction tab. The parameter panel on the left
is shared with the Live tab, so whatever you tune is what runs live. Save it per bird with **Save as...**.

1. **Pick a folder** (Browse..., or type a path and press Enter). The pattern accepts `;`-separated
   wildcards, e.g. `*.wav;*.WAV`. Step through files with Prev / Next / Random, PageUp / PageDown, or
   by clicking the list.
2. **File view**: spectrogram, detection signal (log scale, like LabVIEW) and song detector, with
   linked time axes (scroll to zoom, drag to pan, **Zoom full** to reset).
   - Yellow numbers are syllable durations (ms). Cyan numbers are IOIs, shown purple where the target
     matched. Green labels are the syllable each one was classified as (amber = the one being edited).
   - Drag the **green/red horizontal lines** on the detection plot to set the start/end thresholds.
   - Drag the **band edges** on the spectrogram (green include / red subtract; tick *Song bands* for the
     song detector's bird / non-bird bands).
   - Drag the **duration threshold** (blue) and **ratio threshold** (grey) on the song plot.
   - Hover a syllable for its SD / IOI / previous values. **Ctrl+click** a syllable to centre the
     syllable being edited on it (window widths are kept). **Ctrl+Shift+click** adds it as a new
     syllable and selects it for editing.
   - Changing thresholds updates instantly. Changing bands or gains recomputes the detection signal in
     the background on all CPU cores, which takes a fraction of a second per file.
3. **Folder summary → Analyze folder** computes every file once. After that:
   - **Editing:** one button per syllable with its count over the folder (★ = feedback target). The
     syllable you pick is the one being edited, here and in the left panel.
   - Its trigger type, window mode and enabled flag, and a *Make feedback target* button.
   - Histograms of IOI, SD of the previous syllable and previous IOI for every syllable in the folder,
     with the syllables classified as the edited one in purple. Drag the amber lines to move its window
     (setpoint + eps, or min and max).
   - **Duration** histogram of the syllables classified as the edited one (their own SD, which is not
     part of the definition), with n, mean ± SD and CV. One narrow peak means the windows pick out a
     single syllable type; a wide or double peak means a neighbouring syllable is getting through.
   - **IOI vs SD** scatter: each cluster is one syllable transition in the motif; the amber box is the
     edited syllable's window.
   - A per-file table with counts for the edited syllable and the target, blocked matches and feedback
     (click a row to open the file). For a stereotyped motif you want about one target per rendition
     and none in files with no song.
   - Threshold, syllable and feedback changes re-run every file instantly. Band, gain and song-detector
     changes need another *Analyze folder* (a warning appears).

The offline analysis uses the same code as the real-time detector and is tested to produce bit-identical
results, including the random feedback draws for a given seed.

Command line: `FeedbackPlease.exe bird42.ini --tune D:\songs\bird42 --analyze` opens straight into
tuning. `make_test_wavs.exe <folder>` writes a dozen synthetic recordings to try it on.

## How the live engine works

See [docs/ENGINE.md](docs/ENGINE.md) for a schematic of the threads, one 1 ms frame, and the timing
of a feedback event.

## How the LabVIEW code maps

| LabVIEW | C++ |
|---|---|
| Top-level acquisition loop, 40 samples/iteration, 15 ms circular buffer | `Engine::run` (`src/app/Engine.cpp`), `Detector` ring buffer (`src/core/Detector.cpp`) |
| `DetectBirdSong.vi` + STFT Spectrogram Record (song vs. calls/noise) | `SongDetector` (`src/core/SongDetector.cpp`), `ButterworthBandpass` (`src/core/Filters.cpp`) |
| `DetectSyllables.vi`: mean subtract, software gain, power spectrum, *Spectrum to Amp Multibands*, band combination, to dB, low cutoff | `BandEnvelope` (`src/core/BandEnvelope.cpp`) |
| *Syllable and IOI Time F(x)* (start/end thresholds, IOI, syllable duration) | `SyllableSegmenter` (`src/core/Syllables.cpp`) |
| `CalcDetectSignal.vi` (trigger type, setpoints, eps, IOI n-1 / SD n-1 feedback nodes) and "All Sylls Dig Code" | `SyllableClassifier` (`src/core/SyllableCodes.cpp`) |
| Feedback state machine (Listening, Triggered, Delay, Sending Trigger, Refractory Period) + "Simple Trigger Verification" amplitude check | `FeedbackStateMachine` (`src/core/FeedbackStateMachine.cpp`) |
| DAQ I/O | `DaqBackend` (`src/io/DaqBackend.cpp`) |
| Front panel | `Gui` (`src/app/Gui.cpp`) |

### Detection signal

For each 1 ms frame the last *N* = 15 ms of samples (600 at 40 kHz) are mean-subtracted, multiplied by
the software gain, and Hann-windowed. For every band with a non-zero gain the RMS amplitude in that band
is computed from the power spectrum (only the needed DFT bins are evaluated, using Goertzel). Then:

```
include  = sum(gain_i * bandRMS_i)  over the green (include) rows
subtract = sum(gain_j * bandRMS_j)  over the red (subtract) rows
value    = include - subtract       (Band Combination = Difference; also Sum, Ratio, Include only)
```

With 15 ms windows the bins are 66.7 Hz wide, so band edges are only accurate to about ±67 Hz.

### Syllables and the feedback target

Syllables are the centre of the app. Each one has a name, a U8 code (1–255), a **trigger type** and a
window for each value it uses, given as **setpoint ± eps** or **min / max** (per syllable; switching
converts the values). With onsets A, B, C, at C's onset (as wired in `CalcDetectSignal.vi`):

| Value | Meaning | Used by |
|---|---|---|
| **IOI** | C − B, the interval ending now | IOI · SD, IOI · IOI, SD, IOI |
| **SD** | duration of B, the previous syllable | SD, IOI · IOI, SD, IOI |
| **Prev IOI** | B − A | IOI, SD, IOI |

At every IOI end (each syllable onset after the first of a file/run) the list is checked **top to
bottom** and the **first** syllable that matches is reported. Order matters when windows overlap; use
the ▲▼ buttons.

- **Feedback target** is a dropdown of the syllables (or *none*). Feedback is triggered when an IOI
  end is classified as the target, so the target must be the first match. A broader syllable listed
  above it takes precedence.
- The matched syllable's code is written to the **Syllable code port** and counted ("All Sylls Count",
  shown in each syllable's header while running, logged as `syllable` events). With no match, 0 is
  written. *Write codes to port* turns the port off; classification and feedback keep running.
- **Syllable code port** (Hardware): either an NI MAX task name such as `P2_SyllOut` (loaded with
  `DAQmxLoadTask`, like the LabVIEW VI) or a port / line range such as `Dev2/port1` or
  `Dev2/port1/line0:7` (written with `DAQmxWriteDigitalU8`).
- **Hold**: *Until syllable ends* (default; the code is on the port while the matched syllable lasts,
  my reading of the `= Timing Syllable` comparison), *Until next onset*, or a fixed *Pulse*.
- Only the trigger types evaluated at an onset are offered, since the current syllable's own duration
  isn't known yet at its IOI end.
- **+ Add syllable** copies the syllable being edited. Settings files from before syllables existed
  are converted on load: their target becomes a syllable called "target", which is selected.

Note that measured durations include the detector's lag: the envelope stays above the end threshold
for up to one window (≤15 ms) after a syllable ends, so measured SDs run long and gaps run short,
exactly as in the LabVIEW version.

### Song detection (DetectBirdSong.vi)

A second, slower detector decides whether the bird is *singing*, so calls and cage noise can't
trigger feedback. Its pre-processing is separate (gain, offset, mean subtract, optional Butterworth bandpass
of order N = 2N poles, as in LabVIEW). Every `hop` ms it takes the last `window length` samples
(Hann, 2048 by default), runs an FFT (size = max(window, 2 × frequency bins), rounded up to a power of 2),
and computes:

```
ratio    = mean power per bin in [Bird Low, Bird High]  /  mean power per bin in [0, Non-bird Cutoff]
above    = ratio > Ratio Threshold
fraction = share of the columns in the last Song Duration (0.5 s) that were above   ("Convolved Thingy")
song     = fraction > Duration Threshold (0.25)                                      ("bDetect?")
```

LabVIEW recomputes a whole spectrogram of the buffer every iteration and convolves the 0/1 trace with a
box kernel; the streaming version gives the same value for the newest time point at a fraction of the cost.
"time steps" is replaced by the column hop.

With **Gate feedback on song** enabled (the default), a target match only starts a feedback
trial while `song` is on. Blocked matches are logged as `match ... (blocked: no song)` and drawn as grey
lines. The optional **Song line** DO is high while song is detected.

Note on timing: with a 0.5 s kernel and a 0.25 threshold, song is only recognized after about 125 ms of
singing, and it stays on for roughly 375 ms after singing stops. A target syllable in the first ~125 ms
of a bout will be blocked. Lower the duration threshold or song duration, or add a **Hold**, if that matters.

### Outputs and latency

- **AO feedback waveform.** One finite, committed AO task per run, clocked at the wave file's sample
  rate. The wave is written to the device while the state machine is in its Delay state, so delivery
  is just `DAQmxStartTask` on a committed task (sub-millisecond on PCIe/PXIe X-series). Waves 1 and 2
  are padded to the same length so they can share the task.
- **TTL lines** are on-demand (software-timed) writes, issued in the same frame as the decision.
  Feedback and match lines pulse for `TTL pulse (ms)`; the syllable line is high during syllables.
- **Input → decision latency** is the frame size (1 ms) plus the DAQmx transfer plus processing
  (~0.15 ms average on this PC). The status bar shows the backlog (acquired but unprocessed data) and
  processing time; if backlog grows, the loop is falling behind.

## Things I had to guess

Please check these against the original VIs:

1. **Spectrum to Amp Multibands** (the screenshot was not included). I assumed each row's *Gain*
   is a linear weight on that band's RMS amplitude, gain 0 disables a row, and *Difference* =
   green sum − red sum. With the panel defaults that is `20 × RMS(1–7 kHz) − 1 × RMS(2 Hz–1 kHz)` after
   ×3 software gain. If the VI sums power instead of amplitude, or uses dB, the thresholds won't transfer
   directly. `offline_detect` on a recording that LabVIEW also processed will show this quickly.
2. **display dB?** is treated as display-only; thresholds always compare against the linear value
   (0.069 / 0.027 look like linear values).
3. **Low Cutoff** sets values below the threshold to 0.
4. **Feedback 2**: the panel doesn't say whether feedback 2 is an *alternative* to feedback 1
   (randomly chosen per trial) or a *second stimulus after* feedback 1. Both are implemented
   (`Feedback 2 mode`); Alternative is the default. In Alternative mode a detection gives feedback 1
   with Probability 1, else feedback 2 with Probability 2, else a logged catch trial.
5. **Feedback Delay (ms)** is added on top of Delay 1/2.
6. **Detect Signal Thresh** is checked at the moment of sending: if the envelope has already
   dropped below it, the feedback is suppressed and logged ("Simple Trigger Verification").
7. **Precalced Value IN** (bypassing the spectrum when ≥ 0) is not ported. The C++ always computes.
8. Only parameter set 0 is used (DetectSyllables wires a constant 0 to the param index).
9. **Song ratio** is bird / non-bird mean power per bin (linear). A "Bird / (Bird + Non-bird)" option is
   available in case the VI normalizes differently.
10. **Butterworth Filter?** defaults to off. With a 3–10 kHz bandpass in front of the spectrogram, the
    0–1 kHz non-bird band is filtered away and the ratio becomes meaningless, so the panel LED was most likely off.
11. **How bDetect is used** by the top-level VI is not in the screenshots. Here it gates feedback. The
    "Play Sample?" playback inside DetectBirdSong (a debugging aid) is not ported.

## Changes from the LabVIEW behaviour

- Zero-duration state transitions happen within one frame (LabVIEW spends one loop iteration
  per state, adding ~1 ms per hop between detection and Sending).
- The random draw uses a seeded Mersenne Twister and every trial, catch trial and suppression is
  logged with its sample index to `recordings/session_*_events.csv`, and each song is saved as its own
  WAV with pre-roll (see *Recording*).
- All processing happens on one time-critical thread; the GUI only reads snapshots, so redrawing
  plots cannot stall acquisition.
- If the app crashes, it writes a stack trace to `feedbackplease_crash.txt`.

## Possible improvements

- Minimum syllable duration / minimum gap filters to reject clicks and brief dropouts.
- Reset the IOI history after long silences so the first IOI of a new bout can't match by accident.
- Hardware-timed DO/AO clocked from `ai/SampleClock` if you need sample-locked sync pulses
  (X-series supports this).
- Record the AO and DO signals back on spare AI channels to measure true end-to-end latency.

## Closed loop (spike-triggered feedback)

The **Closed loop** tab is the former stand-alone `ClosedLoop.exe` (from `LiveSorter_BF`) inside this GUI. It
runs on its own threads and SpikeGLX connections, so it can run at the same time as the microphone engine on
the Live tab. Starting or stopping one does not affect the other. The engine writes each matched syllable's code
to its **Syllable code port**; wire that port to the NI lines in **NI syllable lines** (default 5,6,7) that
SpikeGLX records, and the closed loop sees those codes as syllable onsets.

While the tab is selected the left panel shows ClosedLoop's config keys (same names and meaning as
`config.example.txt`), grouped like the file. **Load config... / Export config...** read and write the
`ClosedLoop.exe` config format, so existing files work unchanged. Settings are also saved with the others, next
to the `.ini`, as `<name>.closedloop.txt`.

- **Run calibration**: fits the filter and threshold from the training `.bin` + Kilosort folder (Python backend
  runs `calibrate_for_closedloop.py`; its output goes to the log, **Stop** kills it). Results are written to the
  filter folder; buttons open the diagnostic PNGs.
- **Start live**: with *Calibrate before going live* on, calibrates first, then connects to SpikeGLX (it must be
  acquiring), detects spikes on IMEC and syllable onsets on NI, counts spikes in the window after each onset and
  raises `doLine` per the decision mode. Off: uses the saved filter.
- **Test pulse**: raises `doLine` once, to check wiring.
- Live view: counters, spike count per rendition against the threshold, spike times relative to onset with the
  window and sub-windows marked, a table of recent decisions (with onset-to-pulse latency) and the log.

Build: needs the SpikeGLX SDK (`SGLX_SDK_DIR`, default `../LiveSorter_BF/SDK/API`; `SglxApi.dll` is copied next to
the exe). Without it the tab is left out. `CLOSEDLOOP_FILTERGEN_DIR` sets the default calibration script folder.
The detector code lives in `src/closedloop/` (copied from `LiveSorter_BF/ClosedLoop`, with logging routed to the
GUI and counters/decision callbacks added); `fp_closedloop_test.exe` checks the settings and runner without hardware.
Open directly on the tab with `FeedbackPlease.exe bird.ini --closedloop`.

## MSN finder

The **MSN finder** tab looks through the units of a Kilosort run for ones that fire at a consistent time in a
given syllable. It is based on the first figure cell of the `msnFinder` notebook, drawn live and never saved.

1. Set **Run folder** to the CatGT output folder (e.g. `X:/Choc57_out/catgt_Choc57_d2.0_g0`; it holds the
   `tcat.nidq` files, the sync edge files and `<run>_imec0/imec0_ks4`) and press **Load session**. The first load
   of a run decodes the syllable labels from the NI file, which can take a minute; the result is cached (set
   **Cache folder** to the notebook's `cache` folder to reuse what it already decoded).
2. Pick a **Syllable** label. Units are ranked by the variance of their spike-time histogram (unwarped ms from
   onset to the median syllable duration; *Histogram bins*, *Min spikes*, *Show lowest-variance units* as in the
   notebook), lowest first.
3. Page through the units with **prev / next**, the slider, the arrow keys or the ranking table. Each unit shows
   the mean spectrogram, the syllable-aligned raster (median end and interquartile range in red; the gray band is
   the *Window* centred on the unit's histogram peak), spikes in that window per rendition averaged over
   *Rolling mean* renditions, the template waveform and the unit's position on its shank.

The work is done by `tools/msn_finder/msn_server.py` (numpy + scipy, no pandas), which the app starts as a child
process using the **Python** setting; `msn_data.py` next to it is the notebook's loader. Settings are saved next
to the `.ini` as `<name>.msn.txt`. `FeedbackPlease.exe bird.ini --msn-load` opens the tab and loads the session.

## Closed-loop pulse feedback (white-noise burst)

The **Closed-loop pulse feedback** section of the Live tab's left panel replaces the LabVIEW loop that waited for
the closed loop's digital pulse and played white noise. The closed loop raises a digital line when its spike count
crosses threshold; wire that line to a digital input of the NI card FeedbackPlease uses and enter it as **Pulse
input line** (e.g. `Dev1/port0/line4`; not one of the output lines). A rising edge plays a burst on the **AO
channel** (Hardware / Source), the same speaker output as Wave 1/2:

```
Gaussian white noise (unit variance) x Amplitude gain -> Butterworth bandpass (1500-8000 Hz, order 10)
-> Delay (ms) of silence, then Duration (ms) of noise, clipped to the AO range -> finite AO write at 40 kHz
```

Defaults are the values on the LabVIEW diagram. The panel shows the expected rms after filtering (gain x
sqrt(band / (rate/2))). **Stimulus** can also be a wave file (volts at full scale = gain). A new noise burst is
generated after each one plays, so every trigger is different, and the next one is loaded before the pulse arrives.

- The input is sampled once per 1 ms frame, so pulses must be at least 2 ms wide and add up to 1 ms of latency;
  the burst then starts with a `DAQmxStartTask` on an already committed task, like the other feedback.
- Pulses within **Refractory (ms)** of one that played are counted and ignored. The Live status line shows pulses
  in and bursts played, each pulse is in the events CSV (`pulse in`, `pulse feedback`), and the feedback TTL line
  and the red markers on the plots fire for it too.
- **Test pulse feedback** in the toolbar plays a burst without the input, to check the speaker and levels.
- Settings are applied at Start (the digital input and the AO waveform are built then).
- The Closed loop tab's **Digital line** is a SpikeGLX NI output; it only reaches this input through your wiring.

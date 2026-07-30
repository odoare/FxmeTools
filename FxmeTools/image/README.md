# FxmeTools — image

Image and video input for FX-Mechanics plugins: still pictures, webcams and
video files behind one interface, with colour adjustments, rate limiting and
an analysis-friendly luminance grid. Everything lives in namespace `fxme`
and is pulled in by `#include <FxmeTools/FxmeTools.h>`.

The design goal is that a plugin never talks to V4L2, DirectShow,
AVFoundation or FFmpeg directly: it owns a `fxme::VideoEngine`, sets a few
parameters, and receives processed frames.

## Files

| File | What it gives you |
|---|---|
| `ColorBlobTracker.h` | colour-keyed centroid tracking: weighted centre of gravity of pixels near a reference colour + confidence, one pass, no allocation |
| `FrameSource.h` | `fxme::FrameSource` — the producer interface (implement it for custom inputs) |
| `Homography.h` | 4-point plane homography (DLT, JUCE-free): camera-to-plane calibration from clicked grid corners, `toUnitSquare` / `fromQuad` / `inverted` / `isConvexQuad` |
| `StillImageSource.h` | still picture from a file or from memory |
| `V4l2CameraSource.h/.cpp` | webcam on Linux (V4L2 ioctls, YUYV to ARGB, no extra library) |
| `JuceCameraSource.h` | webcam on Windows/macOS (`juce::CameraDevice`) |
| `VideoFileSource.h/.cpp` | video file decoding with FFmpeg (PTS pacing, loop, pause, position) |
| `VideoEngine.h/.cpp` | owns a source, pumps it, applies adjustments, publishes frames |
| `ImageAdjustments.h` | brightness / contrast / saturation / mirroring, in place |
| `LuminanceGrid.h` | downsampled float luminance field with one frame of history |

## Build setup

Add to the plugin's CMakeLists, after `fxmetools_attach()`:

```cmake
fxmetools_attach_video(MyPlugin)          # cameras + video files (FFmpeg if found)
fxmetools_attach_video(MyPlugin NO_FFMPEG) # cameras only
```

It links `juce_video` on Windows/macOS and defines `FXME_HAS_JUCE_CAMERA`,
finds FFmpeg with pkg-config and defines `FXME_HAS_FFMPEG`, and prints what
it enabled. Nothing is mandatory: without the helper the image classes still
compile, cameras work on Linux (V4L2 needs nothing), and the unsupported
backends report failure instead of breaking the build. `VideoEngine::
isCameraSupported()` / `isVideoFileSupported()` let the UI say so.

## Typical use

```cpp
class MyEditor : public juce::Component, private juce::ChangeListener { ... };

// processor
fxme::VideoEngine engine;

engine.setUpdateRateHz (30);
engine.setWorkingMaxDimension (320);      // small copy for analysis/DSP
engine.setAdjustments ({ .brightness = 0.1f, .contrast = 0.2f,
                         .saturation = 1.0f, .mirrorH = true });

engine.onFrame = [this] (const fxme::VideoEngine::ProcessedFrame& f)
{
    grid.update (f.working);              // fxme::LuminanceGrid
    // ... derive control values, publish them as atomics for the audio thread
};

engine.addChangeListener (editor);        // editor repaints with engine.getImage()

// sources — all return bool
engine.loadImageFile (file);
engine.loadImage (generatedImage, "My pattern");
engine.loadVideoFile (movie);             // then setVideoPaused / getVideoPositionSeconds
for (auto& id : fxme::VideoEngine::getCameraDeviceIds())  engine.openCamera (id);
```

`ProcessedFrame` carries two views of the same frame: `image` at capture
resolution (draw this) and `working` capped to
`setWorkingMaxDimension()` (analyse this). When no cap applies they are the
same object, so nothing is copied.

## Threading contract

* Source management, `onFrame`, `getImage()` and the change broadcast are
  **message thread**.
* `setUpdateRateHz`, `setAdjustments`, `setWorkingMaxDimension` and
  `refresh` are **any thread** (atomic stores) — a plugin can push modulated
  adjustment values from `processBlock` every block.
* Capture and decoding run on their own threads inside the sources; frames
  reach the engine through a one-slot mailbox, so a slow consumer only ever
  drops frames, never blocks the camera.
* The timer reprocesses only when a new frame arrived or a setting changed:
  a still image costs nothing once published.

## Analysis helper

`fxme::LuminanceGrid` samples a frame into a small row-major float grid in
[0, 1] and keeps the previous grid, which is what motion/optical-flow and
region statistics need:

```cpp
fxme::LuminanceGrid grid { 160 };                 // max width in cells
grid.update (frame.working);
if (grid.hasPrevious())
    for (int y = 1; y < grid.getHeight() - 1; ++y)
        for (int x = 1; x < grid.getWidth() - 1; ++x)
        {
            const float ix = (grid.at (x+1, y) - grid.at (x-1, y)) * 0.5f;   // gradients
            const float iy = (grid.at (x, y+1) - grid.at (x, y-1)) * 0.5f;
            const float it =  grid.at (x, y)   - grid.previousAt (x, y);      // motion
        }
```

`Mode::maxChannel` (default) reports saturated colours as bright, which
suits terrain reading; `Mode::rec601` is perceptual luma. Buffers are
reused, so the steady state allocates nothing; a resolution change drops the
history for one frame (`hasPrevious()` reports it).

## Writing a custom source

Implement `fxme::FrameSource` (`start`, `stop`, `getName`, `isLive`, and
call `onFrame` with each new `juce::Image`), then:

```cpp
engine.setSource (std::make_unique<MyNdiSource> (...));
```

The engine handles the rest — rate limiting, adjustments, working copy,
publication. Emit freshly created images and never call `onFrame` after
`stop()` returns.

## Known users

* **FlowSynth** — webcam / video / image terrain for wave-terrain synthesis
  plus shape-local optical-flow analysis (`LuminanceGrid`).
* **ViCo** (predecessor) — the V4L2 capture and flow analysis originate
  there; it can be rebuilt on this module.

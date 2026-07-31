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
| `CameraPose.h` | camera pose from a calibrated plane (homography decomposition) and multi-camera triangulation: where the camera is, and how high above the plane an object is |
| `ColorBlobTracker.h` | colour-keyed blob tracking: brightness-independent (chroma) matching, connected-component labelling, size/shape filters, optional temporal gating and diagnostics |
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

## Colour tracking

`fxme::ColorBlobTracker` follows an object marked with a distinctive
colour: it finds the connected region that best matches a reference and
reports its centroid, normalised to the frame.

```cpp
fxme::ColorBlobTracker tracker;                   // holds reusable buffers

fxme::ColorBlobTracker::Params params;
params.reference = pickedColour;                  // sampled from the image
params.tolerance = 0.15f;
params.minBlobPixels = 12;                        // per connected blob

const auto blob = tracker.track (frame.working, params);
if (blob.found)
    useIt (blob.x, blob.y, blob.confidence);
```

Two defaults matter, both the result of a first version that failed in the
field (a red object lost to a bright area of the scene):

* matching is **brightness-independent** (`MatchMode::chroma`). An RGB
  distance counts a brightness difference as much as a hue difference, so
  the same object in shadow scores worse than an unrelated bright area.
  Chroma mode compares normalised chromaticity and skips pixels that are
  too dark or too grey to have a meaningful hue (`minValue`,
  `minSaturation`). A reference colour that is itself unsaturated falls
  back to RGB, so grey and white targets still work;
* the result is **one connected blob**, not a global centroid. Summing all
  matching pixels lets a large mediocre region outweigh the compact true
  one. Blobs are labelled (two-pass union-find, 8-connectivity), filtered
  by size and compactness (`minBlobPixels`, `maxBlobFraction`,
  `minCompactness`), and scored by colour quality times a saturating size
  term so that past "big enough", quality decides.

Optional `previousX/Y` + `searchRadius` bias the score towards blobs near
the last known position (a soft preference, so a fast object is not lost).
Passing a `Diagnostics` returns every candidate blob and, on request, a
mask image of the match weights, which is what a UI needs to show why a
track went wrong. `track()` is an instance method because the scratch
buffers are members and are reused: the steady state does not allocate.

## Height from several cameras

`fxme::CameraPose` answers the question `Homography` cannot: how far *above*
the plane an object is. Given the calibration homography and a guess at the
lens, it recovers where each camera stands; two such cameras seeing the same
object let its 3D position be triangulated.

```cpp
const auto k = fxme::CameraIntrinsics::fromHorizontalFov (juce::degreesToRadians (60.0),
                                                          imageWidth / (double) imageHeight);

// planeToImage is Homography::toUnitSquare(corners)->inverted()
const auto pose = fxme::CameraPose::fromPlaneHomography (planeToImage, 1.0, 1.0 / gridAspect, k);

fxme::Ray rays[2] { poseA->rayThrough (uA, vA, kA), poseB->rayThrough (uB, vB, kB) };
fxme::CameraPose used[2] { *poseA, *poseB };

if (auto point = fxme::triangulate (rays, 2))
{
    point->z *= fxme::heightSign (used, 2);       // do not skip this
    useHeight (point->z);
}
```

**`heightSign` is not optional.** A homography cannot tell the plane's two
sides apart: the recovered Z axis is `r1 x r2`, so which way it points comes
from the order the plane's corners were given in, not from the scene. Give
the same physical plane its corners the other way round and every camera
lands at negative Z, with objects above the plane reporting negative
heights. Cameras that can see the plane are all on one side of it, and
`heightSign` reads that off them. In-plane X and Y are unaffected.

World units follow the plane size passed in, so giving the plane a width of
1 makes every length a fraction of the grid width.

Accuracy is set by the assumed intrinsics. With exact ones the recovery is
correct to numerical precision; a 10 degree error in the stated field of
view costs roughly 10 percent of the measured height, and it behaves as a
scale error, so trimming the field of view until a known height reads right
is a workable calibration. Feeding measured intrinsics (from a chessboard)
instead is the direct route to better numbers, which is why they are a
parameter rather than baked in.

## Storing a picture in a preset

A file path in a preset breaks as soon as the preset travels to another
machine. `fxme::EmbeddedImage` (in `presets/`, the image counterpart of
`EmbeddedAudio`) puts the pixels themselves into the state ValueTree, so
presets and host sessions are self-contained:

```cpp
// saving — downscales to 1024 px and JPEG-encodes by default
fxme::EmbeddedImage::embed (apvts.state, "terrain", engine.getRawImage(), "clouds.jpg");
fxme::EmbeddedImage::embedFile (apvts.state, "terrain", file);          // straight from disk
fxme::EmbeddedImage::embed (apvts.state, "logo", image, "logo.png",
                            { .maxDimension = 512, .format = fxme::ImageEncoding::png });

// loading
if (auto img = fxme::EmbeddedImage::load (apvts.state, "terrain"); img.isValid())
    engine.loadImage (img, fxme::EmbeddedImage::getEmbeddedName (apvts.state, "terrain"));
```

`getRawImage()` returns the frame *before* adjustments, which is what you
want to store (brightness/contrast/mirror are parameters and travel in the
preset already). Use `getEmbeddedSizeBytes()` to check the cost and
`removeEmbedded()` for sources that must not be embedded — a camera or a
video file keeps its device id or path instead.

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

# Making a plugin FxmeNet-aware

FxmeNet runs a plugin's DSP on a server on the LAN while the plugin's own
editor stays in the DAW, with live meters. It loads the same VST3 twice: a
**source** on the server (processes audio) and a **mirror** in the DAW
(never processes, shows the editor). What the editor reads that is not a
parameter (meters, taps, computed values) travels from the source to the
mirror as telemetry and is written back into the mirror's own objects, so
the editor needs no change. A plugin that is not used under FxmeNet behaves
exactly as before.

The reference is `Examples/GainPan` in the FxmeNet repository; its processor
marks every FxmeNet line with a comment.

## 1. The processor

```cpp
#include <FxmeTools/net/NetBridge.h>

class MyProcessor : public juce::AudioProcessor
{
public:
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return &net; }
    ...
private:
    Compressor compressor;
    fxme::net::NetBridge net;   // after everything it registers
};

MyProcessor::MyProcessor() : ...
{
    net.telemetry().addMeter ("gr", compressor.getGrMeter());
}
```

`NetBridge.h` is not in the FxmeTools umbrella header: include it. It needs
only `juce_audio_processors` and FxmeCore, so a plugin that does not use the
FxmeTools module can include it too (FxmeTools' root on the include path,
FxmeCore on the link line).

If the processor already returns a `VST3ClientExtensions` of its own, keep
it and forward from its `queryIAudioProcessor` to `net.queryInterface (iid, obj)`
when that returns 0.

## 2. What to register

Everything the editor reads from the processor, other than parameters. One
name per object, unique in the plugin, stable across versions (the mirror
skips names it does not know, so renaming one breaks only that channel).

| The editor reads | Register | On the mirror |
| :--- | :--- | :--- |
| an `fxme::VuMeter` | `addMeter (name, meter)` | RMS and peak follow; the held max accumulates until the editor's own `resetMax()` |
| an `fxme::SpectrumTap` or `fxme::WaveformTap` | `addTap (name, tap)` | the samples are pushed into the mirror's tap, so `snapshot()` and the analyzers work unchanged |
| a `std::atomic<float>` | `addValue (name, atomic)` | stored into the same atomic |
| a function that computes a value | `addScalar (name, getter)` | the getter returns `net.telemetry().scalar (name)` when `net.isMirror()` |
| a read-and-reset function ("peak since last asked") | `addPeak (name, takeFunction)` | the function returns `net.telemetry().takePeak (name)` when `net.isMirror()` |

The last two need one line in the getter, because the mirror has no DSP to
compute from:

```cpp
float MyProcessor::getGateOpenness (int slot) const
{
    if (net.isMirror())
        return net.telemetry().scalar (gateNames[slot]);
    return engine.getGateOpenness (slot);
}
```

Taps: under FxmeNet the server enables and disables the source's taps itself
(while the editor is open in the DAW), whatever the plugin set.

## 3. What else the mirror needs

- **Skip heavy work that only feeds the DSP** when `net.isMirror()`:
  background recomputes, IR loading, large allocations. Keep storing the
  atomics the editor reads. The role is set before `prepareToPlay`.
- **Call `net.notifyStateChanged()`** when state that is not a parameter
  changes (an IR loaded, a file chosen). FxmeNet then copies the whole state
  to the server. Parameters need nothing.
- **Keep the editor reading atomics and taps**, never DSP internals (a
  filter's coefficient array, a vector the audio thread resizes). That is the
  house style anyway, and the one thing that makes a port expensive when it is
  not followed.

## 4. Checking it

FxmeNet's `Tools/Loopback` loads a plugin twice in one process, as source and
mirror, and pumps telemetry between them in memory:

```bash
FxmeNetLoopback path/to/MyPlugin.vst3
```

Both editors appear side by side; the mirror's meters must follow the
source's. Switching "Telemetry" off must freeze the mirror's.

## Reference

- `core/FxmeTools/net/TelemetryRegistry.h`: the kinds, the threading rules.
- `core/FxmeTools/net/TelemetryCodec.h`: the byte format.
- `core/FxmeTools/net/NetAbi.h`: the host-side interface (`INetPlugin`,
  `queryNetPlugin()`).
- FxmeNet's `plan.md`: why it is built this way, porting costs measured on
  FxmeFX, Dede and AmbiRR2.

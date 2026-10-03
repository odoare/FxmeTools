# Local (module) presets and the preset widgets: plan

Status: **phases 1 and 2 done** (2026-10-03; in phase 2, only Dede has
moved so far); phases 3 and 4 planned. Written
2026-10-03, from a design discussion while working on Dede. Update this file as phases land,
and record each consumer-visible change in `api-changes.md` as usual.

## Goal

Presets for *part* of a plugin, not only for the whole of it:

- a Dede **spectral region** (one region slot's settings);
- an **FxmeFX effect**, whether it runs as its own plugin (FxmeCompressor) or
  inside a bigger one (the compressor of an FxmeSampler amp chain, an effect
  in MechanOdd). A preset saved in one of these places must load in all the
  others.

Plus two widget improvements that every plugin can use:

- `PresetBarComponent` gets an optional browse button that opens the full
  `PresetComponent` in a callout (what Dede does by hand today);
- both widgets work on global presets and on module presets alike.

Nothing here may break an existing consumer: every new behaviour is opt-in,
and existing call sites compile unchanged.

## Decisions taken (2026-10-03)

1. **What a module preset holds**: every parameter of the module's scope,
   except what describes *where* the instance is or how it is being
   listened to. For a Dede region: everything but the frequency range
   (`flo`, `fhi`), `on`, `solo` and `act`. Gate and ceiling levels, gain and
   pan are included.
2. **Folders** (under the platform user-data directory: `~/.config` on
   Linux, `~/Library/Application Support` on macOS, `%APPDATA%` on Windows):
   - a plugin's global presets: `FX-Mechanics/<PluginName>/Presets/`;
   - a shared module's presets: `FX-Mechanics/Modules/<ModuleName>/Presets/`;
   - Dede's regions follow the module rule even though no other plugin has
     regions: `FX-Mechanics/Modules/SpectralRegion/Presets/`.
3. **A new Dede region** gets fixed defaults that already sound like
   something: a slight delay and a little reverb (values in phase 3). No
   "default region preset" mechanism.
4. **A region's Dry** keeps adding to the global Dry (the band is heard
   twice at 0 dB / 0 dB). Revisit later; out of scope here.

## What exists today

- `fxme::PresetManager` (`FxmeTools/presets/`): whole-state presets. Loads by
  `apvts.replaceState`, keeps the current preset's name on the APVTS root
  (`presetName`, `presetIsFactory`), tracks a dirty flag with a ValueTree
  listener, factory presets from BinaryData (`*_xml` whose root tag is the
  APVTS state type), user presets in `getDefaultUserPresetDirectory
  (product[, subProduct])` = `<user data>/<product>[/<sub>]/Presets`.
  Right for whole presets; it cannot address a subset of parameters, so
  module presets are a separate class, not more options on this one.
- `fxme::PresetBarComponent` (header-only) and `fxme::PresetComponent`: both
  take a `PresetManager&` and use only its lists, load / next / previous,
  save / rename / delete, current name, dirty flag and change broadcasts.
- Users of the widgets: Dede (bar + its own "..." button opening a
  `PresetComponent` in a `CallOutBox`, 320 x 380), AmbiRR2 (bar, and a
  `PresetComponent` in its own overlay), TeAr (bar and browser; it carries
  an older FxmeTools copy under `Source/libs/`, unaffected until bumped).
- FxmeFX uses no preset class at all. Its effects name parameters
  `<prefix>_<Tag>_<Name>`; the standalone plugins use prefix `Main`,
  FxmeSampler gives each chain its own prefix (an amp chain holds EQ, Comp
  and Tube under one prefix). Tags:

  | Effect | Tag | Effect | Tag |
  |---|---|---|---|
  | Cab | `Cab` | Limiter | `Lim` |
  | Chorus | `Chorus` | Oct | `Oct` |
  | Compressor | `Comp` | Phaser | `Phaser` |
  | ConvolReverb | `Rev` | StereoDelay | `Del` |
  | Equalizer | `EQ` | Transient | `Trans` |
  | Flanger | `Flanger` | Tube | `Tube` |
  | Freeze | `Freeze` | | |

  So a module's scope is "every parameter whose ID starts with
  `<prefix>_<Tag>_`", and a preset stores the rest of the ID (`Attack`,
  `Ratio`, ...): the same file then fits every prefix.

- Global preset folders in use (all through `getDefaultUserPresetDirectory`):

  | Project | Folder today (under user data) |
  |---|---|
  | Dede | `Dede/Presets` |
  | AmbiRR2 | `AmbiRR2/Presets` |
  | TeAr | `TeAr/Presets` |
  | Bloom | `Bloom/Presets` |
  | Mango | `Mango/Presets` |
  | MechanOdd | `MechanOdd/Presets` |
  | ModalDish | `ModalDish/Presets` |
  | FlowSynth | `FlowSynth/Presets` |
  | FxmeSampler | `FxmeSampler/<JucePlugin_Name>/Presets` |
  | Localizer | `FXMechanics/Localizer/Presets` (no hyphen) |
  | SuperMoTo | `FXMechanics/SuperMoTo/Presets` (no hyphen) |

## Design

### 1. `fxme::PresetBank`: what the widgets need

A small abstract interface, JUCE side (`FxmeTools/presets/PresetBank.h`),
deriving from `juce::ChangeBroadcaster` (both implementations already
broadcast):

```cpp
class PresetBank : public juce::ChangeBroadcaster
{
public:
    using Preset = fxme::Preset;            // the struct moves out of PresetManager

    virtual const std::vector<Preset>& getFactoryPresets() const = 0;
    virtual const std::vector<Preset>& getUserPresets() const = 0;
    virtual juce::File getUserPresetDirectory() const = 0;
    virtual void rescanUserPresets() = 0;

    virtual bool loadPreset (const Preset&) = 0;
    virtual bool loadNext() = 0;
    virtual bool loadPrevious() = 0;

    virtual bool saveUserPreset (const juce::String& name) = 0;
    virtual bool deleteUserPreset (const Preset&) = 0;
    virtual bool renameUserPreset (const Preset&, const juce::String& newName) = 0;

    virtual juce::String getCurrentPresetName() const = 0;
    virtual bool currentPresetIsFactory() const = 0;
    virtual int  getCurrentFactoryIndex() const = 0;
    virtual int  getCurrentUserIndex() const = 0;
    virtual bool isDirty() const = 0;
};
```

- `PresetManager` derives from it (instead of `ChangeBroadcaster` directly);
  its public API is unchanged. `PresetManager::Preset` stays as an alias of
  `fxme::Preset` so code naming it still compiles.
- `PresetBarComponent` and `PresetComponent` take a `PresetBank&`. Passing
  `processor.getPresetManager()` still works: no call site changes.
- Check every member `PresetComponent.cpp` uses is in the interface (it
  also shows the user folder in `folderLabel`).

### 2. `PresetBarComponent`: optional browse button

Off by default; with it off the bar looks and behaves exactly as now.

```cpp
presetBar.setBrowserButtonVisible (true);      // "..." at the right end, inside the bar
presetBar.setBrowserSize (320, 380);           // the callout's content size (default 320 x 380)
presetBar.setBrowserAccentColour (colour);     // defaults to the bar's accent
```

- The button opens a `PresetComponent` on the same bank in a
  `juce::CallOutBox::launchAsynchronously`, anchored on the button, with the
  plugin editor as parent: `findParentComponentOfClass
  <juce::AudioProcessorEditor>()`, else `getTopLevelComponent()`. A callout
  parented to the desktop misbehaves in some hosts; Dede already parents to
  its editor.
- Tooltip: "Browse, save and delete presets".
- Then Dede's top bar drops its own `presetBrowserButton` and
  `showPresetBrowser()` and turns this on (same look: Dede's "..." uses the
  accent for its text).

### 3. Folders: vendor layout and migration

New helpers on `PresetManager` (or a free function in `presets/`), leaving
`getDefaultUserPresetDirectory` exactly as it is (changing it would silently
orphan every existing user's presets):

```cpp
static constexpr const char* vendorFolderName = "FX-Mechanics";

// <user data>/FX-Mechanics/<pluginName>/Presets
static juce::File getVendorPresetDirectory (const juce::String& pluginName);

// <user data>/FX-Mechanics/Modules/<moduleName>/Presets
static juce::File getModulePresetDirectory (const juce::String& moduleName);
```

Migration, opt-in per plugin when it moves to the vendor folder:

```cpp
presetManager.importLegacyUserPresets (
    PresetManager::getDefaultUserPresetDirectory ("AmbiRR2"));
```

- Copies (does not move) every `*.xml` of the legacy folder that has no
  file of the same name in the new one, then rescans. Copying keeps an
  older installed version of the plugin working with its own folder.
- Runs at most once per legacy folder: a marker file
  (`.imported-to-FX-Mechanics`) is written in the legacy folder, so a preset
  the user deletes in the new folder does not come back.
- Message thread (the manager is constructed on it); quiet on failure (a
  read-only legacy folder just means nothing is imported).

Per project, when it is next worked on (not all at once):

| Project | New folder | Import from |
|---|---|---|
| Dede | `FX-Mechanics/Dede/Presets` | `Dede/Presets` (unreleased: import only for the developer's own presets) |
| AmbiRR2, TeAr, Bloom, Mango, MechanOdd, ModalDish, FlowSynth | `FX-Mechanics/<Name>/Presets` | `<Name>/Presets` |
| FxmeSampler | `FX-Mechanics/<JucePlugin_Name>/Presets` (one per kit build) | `FxmeSampler/<JucePlugin_Name>/Presets` |
| Localizer, SuperMoTo | `FX-Mechanics/<Name>/Presets` | `FXMechanics/<Name>/Presets` |

Open point: FxmeSampler's kits share one product folder today
(`FxmeSampler/<kit>`); under the new rule each kit is its own plugin
(`FX-Mechanics/<kit>`). Fine as long as kit names do not collide with other
plugin names; otherwise use `FX-Mechanics/FxmeSampler/<kit>/Presets`.

### 4. Module presets

Two classes, JUCE side (`FxmeTools/presets/ModulePresetLibrary.h/.cpp`,
`ModulePresetTarget.h/.cpp`). They stay out of `core/`: they are made of
`juce::File`, `juce::XmlElement` and the APVTS.

#### `fxme::ModulePresetLibrary`: one kind of module, shared

Everything that does not depend on a particular instance: the module name,
the user folder, the factory presets, the lists.

```cpp
ModulePresetLibrary (const juce::String& moduleName,           // "Compressor", "SpectralRegion"
                     int formatVersion,                        // see the file format
                     const juce::File& userDirectory,          // usually getModulePresetDirectory (moduleName)
                     const char* const* namedResourceList = nullptr,   // factory presets, as PresetManager
                     int namedResourceListSize = 0,
                     PresetManager::ResourceProvider getNamedResource = nullptr);

const std::vector<Preset>& getFactoryPresets() const;
const std::vector<Preset>& getUserPresets() const;
juce::File getUserDirectory() const;
void rescan();                                      // user folder only

std::unique_ptr<juce::XmlElement> read (const Preset&) const;           // parsed, module checked
bool write (const juce::String& name, const juce::XmlElement& preset);  // creates or overwrites
bool remove (const Preset&);
bool rename (const Preset&, const juce::String& newName);
```

- One library per module kind per plugin instance (Dede: one, for its eight
  region targets). It is a `ChangeBroadcaster`: its targets listen, so a
  preset saved from region 3 shows up in region 5's list at once.
- Several plugin instances (or several plugins) share the folder on disk:
  `rescan()` when a browser opens and before stepping with next / previous,
  so another instance's saves are seen without watching the folder.
- Factory presets: BinaryData `*_xml` resources whose root is
  `FxmeModulePreset` **with the matching `module` attribute**. A plugin can
  embed the factory presets of several modules side by side; each library
  keeps only its own. Global-preset factory files (root tag = the APVTS
  state type) are never mistaken for module presets, and vice versa.

#### `fxme::ModulePresetTarget`: one instance

Binds a library to one instance's parameters, and implements `PresetBank`,
so the bar and the browser work on it unchanged.

```cpp
ModulePresetTarget (ModulePresetLibrary& library,
                    juce::AudioProcessorValueTreeState& apvts,
                    const juce::String& idPrefix,                     // "Main_Comp_", "Pad1_Amp_Comp_", "rg3_"
                    std::function<bool (const juce::String& key)> include = {});   // key = ID minus idPrefix

// Non-parameter settings (an external IR path, an embedded sample):
std::function<void (juce::XmlElement& extra)>       onWriteExtra;  // fill <Extra> at save
std::function<void (const juce::XmlElement* extra)> onReadExtra;   // null when the file has none

// The PresetBank interface, plus:
std::unique_ptr<juce::XmlElement> capture() const;   // the instance as a preset, in memory
bool apply (const juce::XmlElement& preset);         // e.g. a clipboard or an in-memory default
```

- **Scope**: the parameters whose ID starts with `idPrefix` and whose key
  passes `include`. Built once from `apvts.processor.getParameters()` at
  construction (the parameter set never changes at runtime).
- **Load** (`apply`): for every parameter in scope, the file's value for its
  key if there is one, **else its default**: loading is a full reset of the
  module, so an older preset (missing a parameter added since) still gives
  a predictable result. Keys the file has that the scope has not are
  ignored. Each parameter is set as its own gesture
  (`beginChangeGesture` / `setValueNotifyingHost` / `endChangeGesture`).
  Then `onReadExtra`. Then the current name is set, dirty cleared, change
  broadcast.
- **Save**: `capture()`, name it, `library.write()`.
- **Current preset, persisted**: in a child of `apvts.state`,
  `<ModulePresets>` holding one `<Target id="<idPrefix>" name="..."
  factory="0|1"/>` per target. It is saved with the host session and with
  global presets, and a global preset load brings the names back with the
  values. A global preset saved before this exists simply has no
  `<ModulePresets>`: the targets show no name.
- **Dirty**: an APVTS parameter listener on every parameter in scope sets an
  atomic flag and calls `sendChangeMessage()` (async, so safe from the audio
  thread, where automation can arrive). Cleared after a load the same way
  `PresetManager::clearDirtyAsync` does (the APVTS flushes values back from
  the message queue after a change).
- **Global dirty**: loading a module preset changes parameters, so the
  global `PresetManager` marks the global preset modified. That is correct
  (the plugin no longer matches its global preset) and needs nothing.
- **Threading**: everything but the dirty listener runs on the message
  thread. Nothing here is touched by the audio thread.

#### File format

```xml
<FxmeModulePreset module="Compressor" version="1" name="Gentle Glue">
  <Param key="Attack"  value="12.0"/>
  <Param key="Ratio"   value="2.0"/>
  <Param key="Release" value="150.0"/>
  <Extra>
    <!-- whatever onWriteExtra put here -->
  </Extra>
</FxmeModulePreset>
```

- `value` is the parameter's real value (`convertFrom0to1 (getValue())`),
  not the normalised one: a range widened later still reads right.
  Choices are stored as their index, booleans as 0 / 1, as APVTS does.
- `module` must match the library's or the file is refused (a Compressor
  preset never loads into a Tube). `version` is the module's own format
  version, for a future rename of keys: a library can be given a key
  migration table later; until then it is written and checked only for
  being no newer than the library's.
- File name: `juce::File::createLegalFileName (name)` + `.xml`, as
  `PresetManager` does; the display name round-trips through the `name`
  attribute.

## Phases

Each phase ends with every touched consumer building, and an entry in
`api-changes.md`. FxmeTools changes are made in one checkout (the one being
worked in), committed and pushed, then the other projects' submodules are
bumped.

### Phase 1: `PresetBank` and the browse button (FxmeTools; no breaking change)

**Done 2026-10-03.** As planned, with two details settled while doing it:
`PresetBank` also provides `loadFactoryPreset` / `loadUserPreset` (by index;
`PresetComponent` uses them) and the index lookups, so `PresetManager` lost
its own copies of those and of `step`; and the bar's browser rescans the
user folder before opening. Dede builds and uses the button; AmbiRR2 was
built unmodified against it (a scratch copy with its `lib/FxmeTools`
pointing at the new code). Not built: the other consumers, which construct
the widgets from `getPresetManager()` exactly as AmbiRR2 does.

1. Move `Preset` out of `PresetManager` (keep the alias), add `PresetBank`,
   derive `PresetManager` from it.
2. Switch `PresetBarComponent` and `PresetComponent` to `PresetBank&`.
3. Add the optional browse button to `PresetBarComponent`
   (`setBrowserButtonVisible`, `setBrowserSize`, `setBrowserAccentColour`).
4. Verify: build Dede and AmbiRR2 unchanged (they compile against the new
   signatures without edits); open each one's preset UI.
5. Dede: replace its own button and callout with the bar's.

### Phase 2: vendor folders and import (FxmeTools; opt-in)

**Done 2026-10-03** for steps 1 and 2, as designed, on `PresetManager`
(statics `vendorFolderName`, `getVendorPresetDirectory`,
`getModulePresetDirectory`; member `importLegacyUserPresets`, returning the
number copied). One refinement: the marker is written only when every file
was copied, so a partial import (a file that could not be copied) is
retried next time; files already present are always skipped, so a retry
never duplicates. Dede moved, checked on the developer's real folder.
Step 3 (the other projects) stays open: tick them off here as they move.

Moved: Dede.

1. `getVendorPresetDirectory`, `getModulePresetDirectory`,
   `importLegacyUserPresets` (with its marker file).
2. Dede moves to `FX-Mechanics/Dede/Presets`, importing `Dede/Presets`.
3. The other projects move when next worked on (table above), each in its
   own commit with its import line.

### Phase 3: module presets, first client Dede

FxmeTools:

1. `ModulePresetLibrary`, `ModulePresetTarget`, the file format.
2. Tests: a small JUCE console target in FxmeTools (`tests/`, built only on
   request) with an APVTS holding two copies of a fake module under two
   prefixes:
   - save from prefix A, load into prefix B: values equal;
   - a file without some key: that parameter goes to its default;
   - a file with an unknown key: ignored, no error;
   - a file for another module: refused, nothing changed;
   - current name survives `copyState()` / `replaceState()`;
   - dirty set by a parameter change in scope, not by one outside it;
   - factory presets of two modules in one resource list: each library
     lists only its own.

Dede:

3. A `ModulePresetLibrary ("SpectralRegion", 1, getModulePresetDirectory
   ("SpectralRegion"), BinaryData...)` and eight `ModulePresetTarget`s
   (`idPrefix` = `rg<slot>_`, `include` excluding `flo`, `fhi`, `on`,
   `solo`, `act`), owned by the processor (the targets hold the current
   names and dirty flags, which must outlive the editor).
4. The region dock gets a `PresetBarComponent` on its region's target, with
   the browse button. Placement to try first: the strip's header row
   (region chip, then the bar instead of the "Output" label), which is
   level with the tabs' switches and otherwise mostly empty; else the tab
   bar row, right of the tabs.
5. A handful of factory region presets (`Source/Assets/Regions/*.xml`,
   root `FxmeModulePreset module="SpectralRegion"`): e.g. a short slap, a
   dotted-eighth echo, a frozen wash, a tuned resonator (MIDI-note delay),
   a degraded loop. Made by saving from the plugin, then embedded.
6. Ctrl-drag copy (`duplicateRegion`) stays a parameter copy; the copy shows
   the original's preset name (copy the `<Target>` name too) and dirty
   state cleared.
7. **New-region defaults** (decision 3), as the parameters' own defaults,
   so `resetSlotToDefaults` keeps working as it is. Starting values, to be
   tuned by ear:

   | Parameter | Now | Proposed |
   |---|---|---|
   | `mode` | Seconds | DAW sync |
   | `timel` / `timer` | 0.25 / 0.375 | 0.1875 (dotted 1/16) / 0.375 (dotted 1/8) |
   | `fbl` / `fbr` | -12 dB | -14 dB |
   | `fbx` | -inf | -inf |
   | `dry` | 0 dB | 0 dB |
   | `dly` | -inf | -12 dB |
   | `send` | -inf | -18 dB |

   The Delay switch's "bring back" value (`RegionPanel::Session::delayDb`)
   follows the new default. Presets keep their stored values; no state
   version bump (Dede is unreleased).

### Phase 4: FxmeFX, FxmeSampler, MechanOdd

1. FxmeFX: per effect, a library named after the effect (`Compressor`,
   `Equalizer`, ... the effect's class name, not its tag) and a target on
   `Main_<Tag>_`. For a single-effect plugin the module bank **is** its
   preset system: its bar and browser show the module presets, stored in
   `FX-Mechanics/Modules/<Effect>/Presets`. (This is a deliberate exception
   to "a plugin's presets go in `FX-Mechanics/<PluginName>`": sharing
   requires it. To confirm when starting this phase.)
2. Factory presets per effect in FxmeFX (`Source/<Effect>/Presets/*.xml` or
   `Source/Common/Assets/Modules/<Effect>/`), embedded by the standalone
   plugin and by every host plugin that embeds the effect (an FxmeFX CMake
   helper listing them, so FxmeSampler and MechanOdd pick up the same set).
3. ConvolReverb and Cab: their non-parameter state (external IR file)
   through `onWriteExtra` / `onReadExtra`.
4. FxmeSampler: one target per effect per chain (`<chainPrefix>_<Tag>_`),
   a bar in each effect's panel. MechanOdd likewise. UI placement decided
   per plugin.

## Risks and things to watch

- **Order of APVTS parameter listeners vs. preset loads**: the dirty flag
  must be cleared *after* the APVTS flushes the loaded values back (see
  `clearDirtyAsync`); test it with the host open, not only in the test
  target.
- **Many listeners**: Dede has ~60 parameters per region, 8 regions: ~480
  APVTS listeners. Cheap, but each fires on every automation step; the
  listener only sets an atomic and posts one change message when the flag
  flips, never per value.
- **Two plugin instances saving the same module preset name** at once: last
  write wins, as with global presets today. Acceptable.
- **The PresetManager's ValueTree listener** sees the `<ModulePresets>`
  child change on every module load and marks the global preset dirty; that
  is the intended behaviour (see "Global dirty"), but the `<ModulePresets>`
  bookkeeping alone (a rename of the current module preset) should not: set
  it with the same suppression `PresetManager` uses, or keep it out of the
  dirty check by identifier.
- **TeAr's old copy** of FxmeTools (`Source/libs/FxmeTools`) is on the old
  layout; it picks this up only when it moves to the submodule.
- **"FXMechanics" vs "FX-Mechanics"**: Localizer and SuperMoTo used the
  former; the import covers them, and the vendor constant is the single
  spelling from now on.

## Not in this plan

- A region's Dry replacing its range in the global Dry (decision 4).
- Copy / paste of a module between instances through the clipboard
  (`capture()` / `apply()` make it a small addition later).
- Watching preset folders for changes made by other instances.

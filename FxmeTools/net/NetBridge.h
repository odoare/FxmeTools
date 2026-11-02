/*
  ------------------------------------------------------------------------------
    NetBridge.h

    Makes a JUCE plugin FxmeNet-aware: its DSP can then run on an FxmeNet
    server while its own editor, unchanged, runs in the DAW with live meters.
    Nothing changes when the plugin is used normally in a DAW.

    In the processor (four lines, editor untouched):

        #include <FxmeTools/net/NetBridge.h>

        class MyProcessor : public juce::AudioProcessor
        {
            ...
            juce::VST3ClientExtensions* getVST3ClientExtensions() override { return &net; }

        private:
            Compressor compressor;
            fxme::net::NetBridge net;          // declared after what it registers
        };

        // constructor
        net.telemetry().addMeter ("gr", compressor.getGrMeter());

    Register everything the editor reads that is not a parameter (see
    core/FxmeTools/net/TelemetryRegistry.h for the kinds). Then, where needed:

      - net.isMirror(): this instance only shows the editor, its audio runs on
        the server. Skip heavy work that only feeds the DSP (background
        recomputes, IR loading), and let getters of computed values return
        net.telemetry().scalar (...).
      - net.notifyStateChanged(): call it when state that is not a parameter
        changes (an IR loaded, a file chosen), so FxmeNet copies the whole
        state across.

    How the host finds it: FxmeNet asks the plugin's VST3 component for the
    interface fxme::net::netPluginIid; this class answers through
    VST3ClientExtensions::queryIAudioProcessor. A plugin that already returns
    its own VST3ClientExtensions forwards to NetBridge::queryInterface()
    instead of returning the bridge. Other formats (AU, CLAP) are not reached:
    FxmeNet hosts VST3 only.

    Depends only on juce_audio_processors and FxmeCore, so a plugin that does
    not use the FxmeTools module can include it with ${FXMETOOLS_DIR} on its
    include path and FxmeCore on its link line.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <FxmeTools/net/NetAbi.h>
#include <FxmeTools/net/TelemetryRegistry.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace fxme::net
{

class NetBridge : public juce::VST3ClientExtensions
{
public:
    NetBridge() = default;

    /** Register the editor's meters, taps and values here (constructor). */
    TelemetryRegistry& telemetry() noexcept     { return registry; }

    Role getRole() const noexcept               { return role.load(); }
    bool isMirror() const noexcept              { return getRole() == Role::mirror; }
    bool isSource() const noexcept              { return getRole() == Role::source; }

    /** Non-parameter state changed: FxmeNet will copy the whole state. */
    void notifyStateChanged() noexcept          { stateChanges.fetch_add (1); }

    /** Answers the FxmeNet interface id; for plugins with their own
        VST3ClientExtensions to forward to. Returns 0 (kResultOk) on a match. */
    std::int32_t queryInterface (const char* iid, void** obj) noexcept
    {
        if (std::memcmp (iid, netPluginIid, 16) == 0)
        {
            api.addRef();
            *obj = static_cast<INetPlugin*> (&api);
            return resultOk;
        }

        *obj = nullptr;
        return -1;   // what JUCE's default answers: not ours
    }

    //==============================================================================
    int32_t queryIAudioProcessor (const Steinberg::TUID iid, void** obj) override
    {
        return queryInterface (iid, obj);
    }

private:
    //==============================================================================
    /** The object handed to the host. Its lifetime is the processor's, so the
        reference count is not used for deletion. */
    class Api final : public INetPlugin
    {
    public:
        explicit Api (NetBridge& b) : bridge (b) {}

        std::int32_t FXME_NET_API queryInterface (const char iid[16], void** obj) override
        {
            return bridge.queryInterface (iid, obj);
        }

        std::uint32_t FXME_NET_API addRef() override   { return 1; }
        std::uint32_t FXME_NET_API release() override  { return 1; }

        std::uint32_t FXME_NET_API getAbiVersion() override  { return abiVersion; }

        std::int32_t FXME_NET_API getRole() override
        {
            return (std::int32_t) bridge.role.load();
        }

        std::int32_t FXME_NET_API setRole (std::int32_t newRole) override
        {
            if (newRole < (std::int32_t) Role::local || newRole > (std::int32_t) Role::mirror)
                return resultInvalid;
            bridge.role.store ((Role) newRole);
            return resultOk;
        }

        void FXME_NET_API setTelemetryActive (std::int32_t active) override
        {
            if (bridge.isSource())
            {
                const std::lock_guard<std::mutex> lock (bridge.sourceLock);
                bridge.registry.setActive (active != 0);
            }
        }

        std::int32_t FXME_NET_API getTelemetrySchema (std::uint8_t* dest, std::int32_t capacity) override
        {
            std::vector<std::uint8_t> schema;
            bridge.registry.writeSchema (schema);
            return copyOut (schema, dest, capacity);
        }

        std::int32_t FXME_NET_API readTelemetry (std::uint8_t* dest, std::int32_t capacity) override
        {
            if (! bridge.isSource())
                return 0;

            const std::lock_guard<std::mutex> lock (bridge.sourceLock);

            // A frame too big for the last caller's buffer is kept for the
            // next call: serialising again would lose its peaks and samples.
            if (bridge.pending.empty())
                bridge.registry.serialise (bridge.pending);

            const auto result = copyOut (bridge.pending, dest, capacity);
            if (result >= 0)
                bridge.pending.clear();
            return result;
        }

        std::int32_t FXME_NET_API writeTelemetry (const std::uint8_t* src, std::int32_t size) override
        {
            if (! bridge.isMirror() || src == nullptr || size < 0)
                return resultInvalid;

            const std::lock_guard<std::mutex> lock (bridge.mirrorLock);
            return bridge.registry.apply (src, (std::size_t) size) ? resultOk : resultInvalid;
        }

        std::uint32_t FXME_NET_API getStateChangeCount() override
        {
            return bridge.stateChanges.load();
        }

    private:
        static std::int32_t copyOut (const std::vector<std::uint8_t>& bytes,
                                     std::uint8_t* dest, std::int32_t capacity)
        {
            const auto size = (std::int32_t) bytes.size();
            if (dest == nullptr || capacity < size)
                return -size;
            std::memcpy (dest, bytes.data(), bytes.size());
            return size;
        }

        NetBridge& bridge;
    };

    TelemetryRegistry registry;
    std::atomic<Role> role { Role::local };
    std::atomic<std::uint32_t> stateChanges { 0 };

    // The host is expected to call each side from one thread; the locks only
    // make a second caller safe, never the audio thread (it is not involved).
    std::mutex sourceLock, mirrorLock;
    std::vector<std::uint8_t> pending;

    Api api { *this };

    JUCE_DECLARE_NON_COPYABLE (NetBridge)
};

} // namespace fxme::net

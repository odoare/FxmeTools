/*
  ------------------------------------------------------------------------------
    NetAbi.h

    The binary interface between an FxmeNet-aware plugin and the FxmeNet host
    (fxmenet-server, and the FxmeNet client loading the same plugin as a
    mirror). The host reaches it through the plugin's VST3 component:

        void* obj = nullptr;
        component->queryInterface (fxme::net::netPluginIid, &obj);
        auto* net = static_cast<fxme::net::INetPlugin*> (obj);   // nullptr: not aware

    Host and plugin are separate binaries, each linking its own copy of every
    library, so only plain types cross this boundary: integers, floats and
    byte buffers in the TelemetryCodec format. No STL, no framework types.

    INetPlugin is laid out like a VST3 FUnknown (the same three methods
    first, same calling convention, no virtual destructor), which is what a
    queryInterface result is expected to be. That relies on the C++ vtable
    layout being the same on both sides, exactly as VST3 itself does.

    Versioning: methods are only ever appended. A host checks getAbiVersion()
    before calling anything added after version 1.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cstdint>

#if defined (_WIN32)
 #define FXME_NET_API __stdcall    // the VST3 SDK's PLUGIN_API on Windows
#else
 #define FXME_NET_API
#endif

namespace fxme::net
{

constexpr std::uint32_t abiVersion = 1;

/** The interface id, compared as 16 raw bytes. Never change it. */
alignas (4) constexpr char netPluginIid[16] = {
    'F', 'X', 'M', 'E', 'N', 'E', 'T', '-',
    (char) 0x5a, (char) 0x0c, (char) 0x41, (char) 0x9e,
    (char) 0x83, (char) 0x27, (char) 0xd6, (char) 0x01
};

/** What a plugin instance is doing. */
enum class Role : std::int32_t
{
    local  = 0,   ///< an ordinary plugin in a DAW (the default)
    source = 1,   ///< on the server: processes audio, telemetry is read from it
    mirror = 2    ///< in the DAW under FxmeNet: never processes, telemetry is written into it
};

/** Return codes of the int32 methods below. */
constexpr std::int32_t resultOk           = 0;
constexpr std::int32_t resultInvalid      = 1;

/** A VST3 FUnknown seen through its three methods (same order, same
    convention), so a host can query a component without the VST3 SDK. */
class UnknownCompatible
{
public:
    virtual std::int32_t  FXME_NET_API queryInterface (const char iid[16], void** obj) = 0;
    virtual std::uint32_t FXME_NET_API addRef() = 0;
    virtual std::uint32_t FXME_NET_API release() = 0;

protected:
    ~UnknownCompatible() = default;
};

class INetPlugin : public UnknownCompatible
{
public:
    //==============================================================================
    // Version 1.

    virtual std::uint32_t FXME_NET_API getAbiVersion() = 0;

    /** Role, as a Role value. setRole returns resultInvalid for an unknown one. */
    virtual std::int32_t FXME_NET_API getRole() = 0;
    virtual std::int32_t FXME_NET_API setRole (std::int32_t role) = 0;

    /** Source: start (nonzero) or stop producing telemetry (the client's
        editor for this instance opened or closed). */
    virtual void FXME_NET_API setTelemetryActive (std::int32_t active) = 0;

    /** Copies the schema into `dest`. Returns the bytes written, or minus the
        size needed when `capacity` is too small (nothing written). */
    virtual std::int32_t FXME_NET_API getTelemetrySchema (std::uint8_t* dest, std::int32_t capacity) = 0;

    /** Source: copies the next telemetry frame into `dest`. Returns the bytes
        written, or minus the size needed when `capacity` is too small; the
        frame is then kept and returned by the next call, so nothing is lost. */
    virtual std::int32_t FXME_NET_API readTelemetry (std::uint8_t* dest, std::int32_t capacity) = 0;

    /** Mirror: writes a frame into the plugin's telemetry objects. */
    virtual std::int32_t FXME_NET_API writeTelemetry (const std::uint8_t* src, std::int32_t size) = 0;

    /** Counts the plugin's notifyStateChanged() calls: a host polling it sees
        a change of non-parameter state and resynchronises the whole state. */
    virtual std::uint32_t FXME_NET_API getStateChangeCount() = 0;

protected:
    ~INetPlugin() = default;
};

/** Host side: asks a VST3 component (any FUnknown pointer, such as JUCE's
    getVST3Client()->getIComponentPtr()) for the FxmeNet interface. Returns
    nullptr when the plugin is not FxmeNet-aware. Call release() on the
    result when done with it. */
inline INetPlugin* queryNetPlugin (void* vst3Unknown) noexcept
{
    if (vst3Unknown == nullptr)
        return nullptr;

    void* obj = nullptr;
    if (static_cast<UnknownCompatible*> (vst3Unknown)->queryInterface (netPluginIid, &obj) != resultOk)
        return nullptr;
    return static_cast<INetPlugin*> (obj);
}

} // namespace fxme::net

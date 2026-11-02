/*
  ------------------------------------------------------------------------------
    TelemetryCodec.h

    The byte format FxmeNet uses to carry what a plugin editor draws (meters,
    taps, a few computed values) from the machine running the DSP to the one
    showing the editor. Shared by the plugin side (TelemetryRegistry, through
    NetBridge), fxmenet-server and the FxmeNet client, so all three agree by
    construction.

    Everything is little-endian, written and read byte by byte, so the format
    does not depend on the host's endianness or struct packing.

    Frame (one per telemetry tick, version 1):

        u32 magic 'FXTL'   u16 version   u16 channel count
        per channel:
            u32 id (idOf (name))   u8 kind   u8 flags (0)   u16 reserved (0)
            u32 payload bytes      payload

    Payload by kind:
        meter   f32 rms, f32 peak, f32 max   (linear gains; max is the held
                                              max taken since the last frame)
        value, scalar, peak   f32
        tap     u64 index of the first sample, u32 count, f32 x count

    Schema (sent once, describes the channels; version 1):

        u32 magic 'FXTS'   u16 version   u16 channel count
        per channel: u32 id   u8 kind   u8 name length   name bytes

    A reader skips channels it does not know (by id or by kind) using the
    payload size, so a plugin version that adds a channel stays readable by
    an older one.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fxme::net
{

//==============================================================================
constexpr std::uint32_t frameMagic    = 0x4C545846u;   // bytes 'F' 'X' 'T' 'L'
constexpr std::uint32_t schemaMagic   = 0x53545846u;   // bytes 'F' 'X' 'T' 'S'
constexpr std::uint16_t codecVersion  = 1;

/** What a telemetry channel carries (see the payload table above). */
enum class ChannelKind : std::uint8_t
{
    meter  = 1,
    value  = 2,
    scalar = 3,
    peak   = 4,
    tap    = 5
};

/** The wire id of a channel name: 32-bit FNV-1a of its bytes. */
constexpr std::uint32_t idOf (const char* text, std::size_t length) noexcept
{
    std::uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < length; ++i)
    {
        h ^= (std::uint8_t) text[i];
        h *= 16777619u;
    }
    return h;
}

constexpr std::uint32_t idOf (const char* nullTerminated) noexcept
{
    std::size_t n = 0;
    while (nullTerminated[n] != 0)
        ++n;
    return idOf (nullTerminated, n);
}

//==============================================================================
/** Appends little-endian values to a byte vector. */
class ByteWriter
{
public:
    explicit ByteWriter (std::vector<std::uint8_t>& destination) : out (destination) {}

    void u8 (std::uint8_t v)    { out.push_back (v); }
    void u16 (std::uint16_t v)  { put (v, 2); }
    void u32 (std::uint32_t v)  { put (v, 4); }
    void u64 (std::uint64_t v)  { put (v, 8); }

    void f32 (float v)
    {
        std::uint32_t bits;
        std::memcpy (&bits, &v, 4);
        u32 (bits);
    }

    void bytes (const void* data, std::size_t n)
    {
        const auto* p = static_cast<const std::uint8_t*> (data);
        out.insert (out.end(), p, p + n);
    }

    /** Current size; with patchU32() fills in a length written ahead. */
    std::size_t position() const noexcept  { return out.size(); }

    void patchU32 (std::size_t at, std::uint32_t v) noexcept
    {
        for (int i = 0; i < 4; ++i)
            out[at + (std::size_t) i] = (std::uint8_t) (v >> (8 * i));
    }

private:
    void put (std::uint64_t v, int n)
    {
        for (int i = 0; i < n; ++i)
            out.push_back ((std::uint8_t) (v >> (8 * i)));
    }

    std::vector<std::uint8_t>& out;
};

//==============================================================================
/** Reads little-endian values; any read past the end sets a sticky failure
    flag and returns zero, so a caller checks ok() once at the end. */
class ByteReader
{
public:
    ByteReader (const std::uint8_t* data, std::size_t size) noexcept : p (data), n (size) {}

    bool ok() const noexcept              { return good; }
    std::size_t remaining() const noexcept { return n - pos; }

    std::uint8_t  u8()   noexcept { return (std::uint8_t)  get (1); }
    std::uint16_t u16()  noexcept { return (std::uint16_t) get (2); }
    std::uint32_t u32()  noexcept { return (std::uint32_t) get (4); }
    std::uint64_t u64()  noexcept { return get (8); }

    float f32() noexcept
    {
        const std::uint32_t bits = u32();
        float v;
        std::memcpy (&v, &bits, 4);
        return v;
    }

    /** Points at the next `count` bytes and skips them (nullptr if short). */
    const std::uint8_t* take (std::size_t count) noexcept
    {
        if (! require (count))
            return nullptr;
        const auto* at = p + pos;
        pos += count;
        return at;
    }

    void skip (std::size_t count) noexcept  { take (count); }

private:
    bool require (std::size_t count) noexcept
    {
        if (good && count <= n - pos)
            return true;
        good = false;
        pos = n;
        return false;
    }

    std::uint64_t get (int count) noexcept
    {
        if (! require ((std::size_t) count))
            return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < count; ++i)
            v |= (std::uint64_t) p[pos + (std::size_t) i] << (8 * i);
        pos += (std::size_t) count;
        return v;
    }

    const std::uint8_t* p;
    std::size_t n;
    std::size_t pos = 0;
    bool good = true;
};

} // namespace fxme::net

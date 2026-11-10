/*
  ------------------------------------------------------------------------------
    ModelData.h

    A neural amp model as data: the file's bytes, kept as they are (in a
    session, a preset), and what its metadata says.

      ModelData   the bytes and the file's name (its extension says the
                  format: .nam for NAM's WaveNet and LSTM, .json for Keras
                  models, read through RTNeural).
      ModelInfo   what the file tells about itself, read without building
                  the network (ModelLoader::readInfo), and completed by the
                  loader (backend, receptive field).

    Plain data, no backend type: safe to copy, store, compare, send to any
    thread.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace fxme::neural
{

struct ModelInfo
{
    // From the metadata (empty when absent).
    std::string name, modeledBy, gearMake, gearModel, gearType, toneType;
    std::string architecture;    // "WaveNet", "LSTM", "SlimmableContainer", ...
    std::string version;         // NAM's file format version
    std::string sizeClass;       // NAM's names, from the config: "Nano", "Feather",
                                 // "Lite", "Standard" (A1 WaveNets), "A2",
                                 // "LSTM 1x16"...; "" when unknown

    double sampleRate = 48000.0; // the rate the model was trained at

    // Calibration, when the file has it.
    std::optional<float> loudnessDb;      // the output's loudness for NAM's standard input
    std::optional<float> inputLevelDbu;   // the dBu level of a full-scale input at capture
    std::optional<float> outputLevelDbu;

    // Filled by the loader (unknown from the metadata alone).
    std::string backend;         // "Internal", "NAM Core", "RTNeural"
    bool isStatic = false;       // NeuralAudio's compiled-in fast path
    int receptiveField = -1;     // samples at the model's rate, -1 for recurrent models

    /** A rough CPU class for the GUI: 0 light (Nano, Feather, LSTMs), 1
        medium (Lite), 2 heavy (Standard, A2), -1 unknown. */
    int cpuClass() const
    {
        if (sizeClass == "Nano" || sizeClass == "Feather" || sizeClass.rfind ("LSTM", 0) == 0)
            return 0;
        if (sizeClass == "Lite")
            return 1;
        if (sizeClass == "Standard" || sizeClass == "A2")
            return 2;
        return -1;
    }

    /** The name to show: the metadata's name, else the gear, else "". */
    std::string displayName() const
    {
        if (! name.empty())
            return name;
        if (! gearMake.empty() || ! gearModel.empty())
            return gearMake + (gearMake.empty() || gearModel.empty() ? "" : " ") + gearModel;
        return {};
    }
};

struct ModelData
{
    std::string fileName;        // without the directory; its extension says the format
    std::vector<char> bytes;

    bool isEmpty() const noexcept { return bytes.empty(); }

    /** ".nam", ".json", ... (lower case), ".nam" when the name has none. */
    std::string extension() const
    {
        const auto dot = fileName.find_last_of ('.');
        if (dot == std::string::npos)
            return ".nam";
        std::string e = fileName.substr (dot);
        for (auto& c : e)
            if (c >= 'A' && c <= 'Z')
                c = (char) (c - 'A' + 'a');
        return e;
    }

    /** Reads a model file. Empty when it cannot be read. */
    static ModelData fromFile (const std::string& path)
    {
        ModelData d;
        std::ifstream f (path, std::ios::binary);
        if (! f)
            return d;
        d.bytes.assign (std::istreambuf_iterator<char> (f), std::istreambuf_iterator<char>());
        const auto slash = path.find_last_of ("/\\");
        d.fileName = slash == std::string::npos ? path : path.substr (slash + 1);
        return d;
    }

    bool operator== (const ModelData& o) const { return fileName == o.fileName && bytes == o.bytes; }
    bool operator!= (const ModelData& o) const { return ! (*this == o); }
};

} // namespace fxme::neural

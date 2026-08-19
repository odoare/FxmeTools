/*
  ==============================================================================

    CracksGenerator.h
    Created: 18 Jan 2024 3:43:48pm
    Author:  od

    Random click / crackle generator. Emits a sparse stream of bipolar impulses
    whose average rate is set by setDensity() (clicks per minute).

  ==============================================================================
*/

#pragma once

#include <FxmeTools/util/ProcessSpec.h>
#include <FxmeTools/util/Random.h>

namespace fxme
{

class CracksGenerator
{
private:
  Random rnd;

  // Density of clicks as number of clicks per minute
  int density;
  int sampleRateMin;

public:

  CracksGenerator(/* args */);
  ~CracksGenerator();

  void prepare(ProcessSpec spec);
  float nextSample();
  void setDensity(int d);
};

} // namespace fxme

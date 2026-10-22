#pragma once
// The stimulus played when a closed-loop pulse arrives (LabVIEW: white noise x amplitude gain, then a
// Butterworth bandpass, written to the speaker AO channel as a finite waveform).

#include <cstdint>
#include <string>
#include <vector>

#include "core/Params.h"

namespace fp {

// One burst at `rate` Hz: delayMs of silence, then durationMs of Gaussian white noise (unit standard
// deviation) x gain, bandpassed between lowHz and highHz (Butterworth, `filterOrder` biquad sections),
// clipped to [vMin, vMax]. Returns an empty vector (and sets *err) if the band is not valid for this rate.
std::vector<double> makeNoiseBurst(const PulseFeedbackParams& p, double rate, double vMin, double vMax, uint32_t seed,
                                   std::string* err = nullptr);

// A wave file played instead of noise: delayMs of silence, then the samples x gain, clipped.
std::vector<double> makeWaveBurst(const std::vector<float>& samples, double rate, const PulseFeedbackParams& p, double vMin,
                                  double vMax);

// Expected RMS (V) of the filtered noise before clipping, from the fraction of the spectrum the band covers.
double expectedNoiseRms(const PulseFeedbackParams& p, double rate);

}  // namespace fp

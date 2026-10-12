#pragma once

#include <vector>

// The reference DI of level-matched auditioning (v0.3 Task B, docs/PRESET_SCHEMA.md "Level matching").
//
// A deterministic, guitar-shaped DI signal generated in code: ~10 s at 48 kHz, mono, palm-muted chugs on a low string,
// open power chords that ring out, a tremolo burst. Every note is a Karplus-Strong string (period in whole samples, a loss
// per period, seeded excitation), so the generator uses only +, -, * and / on doubles (no libm calls): the result is
// bit-identical on every IEEE-754 machine whatever the compiler, and the plugin, `tonerender` and `compute_trims` all
// call this one function. Peak is exactly kReferenceDiPeakDbfs up to rounding.
//
// Why not the NAM export's held-out signal (`DI_BUILTIN`, match/sawblade_match/export/signal.py)? That one is built
// with numpy (SeedSequence, FFT-shaped noise, scipy filters) and is not reproducible from C++; it also holds noise
// steps and sweeps, which are not a DI. Changing anything here bumps kReferenceDiVersion, which invalidates every
// stored autoTrimDb (the version is part of the staleness hash).
namespace sawblade {

constexpr int kReferenceDiVersion = 1;
constexpr double kReferenceDiRate = 48000.0;
constexpr double kReferenceDiSeconds = 10.0;
constexpr double kReferenceDiPeakDbfs = -10.0;

// kReferenceDiSeconds * kReferenceDiRate samples; generated once per process (thread-safe).
const std::vector<float>& referenceDi();

}  // namespace sawblade

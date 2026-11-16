#pragma once

#include "earshot/earshot.h"
#include "earshot/testing.h"

#include <cstdint>

/**
	Tempo: pattern v0.1.1's detector (github.com/stoatworks-labs/pattern,
	`Tracker::DetectTempo`, written up in its AGENTS.md "The detected tempo,
	v0.1.1"), fed from earshot's own audio-hop flux instead of the host's
	per-frame bins.

	## Why not one autocorrelation of the whole envelope

	pattern v0.1.0 summed every bin's flux into one envelope and took the best
	autocorrelation lag between 60 and 200 BPM, preferring half of it when it
	scored 70 % as well. On its release video's four-on-the-floor at 125 it
	read 62. Measured causes, from pattern's write-up: loudness decides the
	metre on a raw sum (a kick that dwarfs the clap repeats at the half note);
	the half-lag test cannot see through a kick-clap cross term; the frame rate
	moves it; and a half-sample period loses to its double on an integer grid.

	## What runs here

	- **Three registers**, low (< 400 Hz), mid (400 Hz .. 5 kHz), high, each a
	  log-flux envelope resampled by area onto a 100 Hz grid (so 44.1 and
	  48 kHz, and every block size and frame rate, see the same grid),
	  smoothed by a 20 ms Gaussian, standardised to unit variance (the clap
	  counts as much as the kick) and weighted 1, 1, 0.5 times the register's
	  own best autocorrelation (noise drops out).
	- **S(T) = 0.5 R(T/2) + R(T) + R(2T)**: a beat is grouped and subdivided.
	- **Family, then level**: the best S under a log-Gaussian prior (120 BPM,
	  one octave) picks the family, which removes the 4/3 and 5/4 relatives;
	  then the FASTEST member (x1/4 .. x4, 60..200 BPM) whose S is at least
	  0.70 of the family's best, with no prior.
	- **Refined** by a parabola on R at the largest multiple of the lag that
	  fits (up to four), divided back down.
	- **Published** once two readings in a row agree within 2 %. One reading a
	  second over the last six seconds, so the first tempo appears at about
	  four seconds.
	- **Only with onsets** (earshot's addition): a reading is taken only if at
	  least six hops in the last six seconds fired an onset (60 BPM, one per
	  beat). Standardising each register to unit variance amplifies anything,
	  and a held chord's partials beating in the flux settled at 167 BPM with
	  confidence 0.71. A published tempo stays published through a breakdown;
	  it is replaced only by two new agreeing readings.

	The constants are pattern's, unchanged; the input and the onset gate are
	the differences.

	**Latency**: 4 s to settle; then one reading a second. **Thread**: render
	thread, inside Engine::update; the detection itself runs once per audio
	second (about 1 ms). No allocation at all: fixed arrays.
*/
namespace earshot
{

class TempoTracker
{
public:
	static constexpr int kRate    = 100;   ///< Hz, the envelope grid
	static constexpr int kEnvSize = 1024;  ///< cells kept (10.24 s)

	void reset() noexcept;

	/// One hop's register flux, the hop ending at audio time `t`; `hopRate` in
	/// Hz; `onset` if any band fired an onset on this hop.
	void push( double t, const double* reg, double hopRate, bool onset ) noexcept;

	void publish( Tempo& out ) const noexcept;

	const testing::Mutations* mut = nullptr;

	// For the harness.
	double candidateBpm() const noexcept { return candidate_; }
	double detectedBpm() const noexcept { return detected_; }
	int    readings() const noexcept { return readings_; }

private:
	void finishCell( int64_t cell ) noexcept;
	void detect() noexcept;
	void detectLegacy() noexcept;

	double  env_[ 3 ][ kEnvSize ] = {};
	double  pending_[ 3 ] = {};       ///< the cell being accumulated
	int64_t pendingCell_  = -1;
	int64_t lastCell_     = -1;       ///< last cell finished
	int64_t lastSecond_   = -1;
	double  detected_     = 0.0;
	double  candidate_    = 0.0;
	double  peakR_        = 0.0;
	int     readings_     = 0;
	double  onsets_[ 64 ] = {};       ///< times of recent onset hops (a ring)
	int     onsetHead_    = 0;
	int     onsetCount_   = 0;
};

} // namespace earshot

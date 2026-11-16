#pragma once

// estest's scaffolding: the configuration matrix, the report, and a driver
// that plays a signal into an Engine the way a host would (audio in blocks,
// update() on video frames).

#include "earshot/earshot.h"
#include "earshot/testing.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace estest
{

using earshot::Config;
using earshot::Engine;
using earshot::Frame;
using earshot::testing::Mutations;

/// One cell of "would this hold at another sample rate and block size?".
struct Run
{
	int    fs    = 48000;
	int    block = 128;
	double fps   = 60.0;
	std::string name() const;
};

/// 44.1 and 48 kHz, each at a power-of-two block and an awkward prime one.
const std::vector< Run >& matrix();

//---------------------------------------------------------------------------

struct Outcome
{
	std::string check;
	std::string run;
	bool        pass = false;
	std::string detail;
};

class Report
{
public:
	/// Record one result. `check` is a stable id ("pitch.sine"), `run` the cell.
	void add( const std::string& check, const std::string& run, bool pass, const char* fmt, ... );
	/// A figure for the summary table (worst case across cells is kept per key).
	void figure( const std::string& key, double value, bool keepMax = true );
	void note( const char* fmt, ... );

	bool quiet = false;   ///< negative mode: collect, do not print every line

	const std::vector< Outcome >& outcomes() const { return outcomes_; }
	int  passed() const;
	int  failed() const;
	/// Did `check` fail in any cell / in every cell where it ran?
	bool failedAnywhere( const std::string& check ) const;
	int  cellsRun( const std::string& check ) const;
	int  cellsFailed( const std::string& check ) const;
	void printFigures() const;
	void clear();

private:
	std::vector< Outcome >                        outcomes_;
	std::map< std::string, std::pair< double, bool > > figures_;
};

std::string format( const char* fmt, ... );

//---------------------------------------------------------------------------

/// Interleaved float audio at a sample rate.
struct Signal
{
	int                  fs = 48000;
	int                  channels = 1;
	std::vector< float > x;   ///< interleaved

	size_t frames() const { return channels > 0 ? x.size() / (size_t)channels : 0; }
	double seconds() const { return (double)frames() / fs; }
};

/// Drives an Engine from a Signal. Constructs the Engine with no capture
/// backend: everything goes through feed(), as a plugin's own source would.
class Driver
{
public:
	Driver( const Run& run, const Config& config = Config(), const Mutations& mut = Mutations() );

	Engine& engine() { return *engine_; }
	const Run& run() const { return run_; }

	/// Play `signal` from the start of a fresh stream (feedBegin), calling
	/// update() at every video frame time. `onFrame( frame, videoTime )`.
	/// `hostFft` (64 bins) if set is passed on every update.
	using FrameFn = std::function< void( const Frame&, double ) >;
	void play( const Signal& signal, const FrameFn& onFrame, bool begin = true );

	/// Continue the same stream with more audio (no feedBegin).
	void resume( const Signal& signal, const FrameFn& onFrame ) { play( signal, onFrame, false ); }

	/// Audio fed since feedBegin, in seconds.
	double fedSeconds() const { return (double)fed_ / run_.fs; }

	/// Collect the tap on every frame.
	bool collectTap = false;
	std::vector< float > tapL, tapR;

	/// Count allocations on this thread inside feed() and update().
	bool countAllocations = false;
	uint64_t allocations = 0;
	double   maxUpdateMs = 0.0, totalUpdateMs = 0.0;
	int      updates = 0;

private:
	Run                       run_;
	std::unique_ptr< Engine > engine_;
	uint64_t                  fed_      = 0;
	uint64_t                  frameIdx_ = 0;
	int                       channels_ = 0;
};

//---------------------------------------------------------------------------
// Allocation counting (estest replaces global operator new)
//---------------------------------------------------------------------------

void     startCountingAllocations();
uint64_t stopCountingAllocations();

double nowMs();

inline double centsBetween( double hz, double ref ) { return 1200.0 * std::log2( hz / ref ); }

//---------------------------------------------------------------------------
// Groups
//---------------------------------------------------------------------------

using GroupFn = std::function< void( Report&, const Run&, const Mutations& ) >;

struct Group
{
	std::string name;     ///< "--pitch" without the dashes
	std::string what;
	GroupFn     fn;
	bool        perRun = true;   ///< false: runs once, ignores the matrix cell
};

const std::vector< Group >& groups();

// Defined in the check files.
void checkFft( Report&, const Run&, const Mutations& );
void checkPitch( Report&, const Run&, const Mutations& );
void checkPartials( Report&, const Run&, const Mutations& );
void checkOnsetPrime( Report&, const Run&, const Mutations& );
void checkOnsetAttack( Report&, const Run&, const Mutations& );
void checkBands( Report&, const Run&, const Mutations& );
void checkThirds( Report&, const Run&, const Mutations& );
void checkTempo( Report&, const Run&, const Mutations& );
void checkStereo( Report&, const Run&, const Mutations& );
void checkTap( Report&, const Run&, const Mutations& );
void checkSilent( Report&, const Run&, const Mutations& );
void checkRing( Report&, const Run&, const Mutations& );
void checkParams( Report&, const Run&, const Mutations& );
void checkFallback( Report&, const Run&, const Mutations& );
void checkCapture( Report&, const Run&, const Mutations& );
void checkAlloc( Report&, const Run&, const Mutations& );

} // namespace estest

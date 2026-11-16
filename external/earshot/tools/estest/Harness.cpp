#include "Harness.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>

//---------------------------------------------------------------------------
// Allocation counting. Replacing the global operator new is the only way to
// see an allocation made inside the library without instrumenting it; the
// count is per thread so a worker thread's allocations are not charged to
// the render thread.
//---------------------------------------------------------------------------

namespace
{
thread_local bool     tCounting = false;
thread_local uint64_t tCount    = 0;

void* allocate( std::size_t size )
{
	if( tCounting )
		++tCount;
	void* p = std::malloc( size == 0 ? 1 : size );
	if( p == nullptr )
		throw std::bad_alloc();
	return p;
}
} // namespace

void* operator new( std::size_t size ) { return allocate( size ); }
void* operator new[]( std::size_t size ) { return allocate( size ); }
void* operator new( std::size_t size, const std::nothrow_t& ) noexcept
{
	if( tCounting )
		++tCount;
	return std::malloc( size == 0 ? 1 : size );
}
void* operator new[]( std::size_t size, const std::nothrow_t& ) noexcept
{
	if( tCounting )
		++tCount;
	return std::malloc( size == 0 ? 1 : size );
}
void operator delete( void* p ) noexcept { std::free( p ); }
void operator delete[]( void* p ) noexcept { std::free( p ); }
void operator delete( void* p, std::size_t ) noexcept { std::free( p ); }
void operator delete[]( void* p, std::size_t ) noexcept { std::free( p ); }
void operator delete( void* p, const std::nothrow_t& ) noexcept { std::free( p ); }
void operator delete[]( void* p, const std::nothrow_t& ) noexcept { std::free( p ); }

namespace estest
{

void startCountingAllocations()
{
	tCount    = 0;
	tCounting = true;
}

uint64_t stopCountingAllocations()
{
	tCounting = false;
	return tCount;
}

double nowMs()
{
	using namespace std::chrono;
	return duration< double, std::milli >( steady_clock::now().time_since_epoch() ).count();
}

//---------------------------------------------------------------------------

std::string Run::name() const
{
	char buf[ 64 ];
	if( fps == 60.0 )
		std::snprintf( buf, sizeof( buf ), "%.1fk/%d", fs / 1000.0, block );
	else
		std::snprintf( buf, sizeof( buf ), "%.1fk/%d@%gfps", fs / 1000.0, block, fps );
	return buf;
}

const std::vector< Run >& matrix()
{
	// 128 is a typical low-latency device buffer; 1031 is prime, so it never
	// lines up with a hop, a window or a video frame.
	static const std::vector< Run > m = {
		{ 44100, 128, 60.0 },
		{ 44100, 1031, 60.0 },
		{ 48000, 128, 60.0 },
		{ 48000, 1031, 60.0 },
	};
	return m;
}

std::string format( const char* fmt, ... )
{
	char    buf[ 1024 ];
	va_list ap;
	va_start( ap, fmt );
	std::vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	return buf;
}

void Report::add( const std::string& check, const std::string& run, bool pass, const char* fmt, ... )
{
	char    buf[ 1024 ];
	va_list ap;
	va_start( ap, fmt );
	std::vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	outcomes_.push_back( { check, run, pass, buf } );
	if( !quiet )
		std::printf( "  %s  %-26s %-14s %s\n", pass ? "ok  " : "FAIL", check.c_str(), run.c_str(), buf );
}

void Report::figure( const std::string& key, double value, bool keepMax )
{
	auto it = figures_.find( key );
	if( it == figures_.end() )
		figures_[ key ] = { value, keepMax };
	else if( keepMax ? value > it->second.first : value < it->second.first )
		it->second.first = value;
}

void Report::note( const char* fmt, ... )
{
	if( quiet )
		return;
	va_list ap;
	va_start( ap, fmt );
	std::printf( "        " );
	std::vprintf( fmt, ap );
	std::printf( "\n" );
	va_end( ap );
}

int Report::passed() const
{
	return (int)std::count_if( outcomes_.begin(), outcomes_.end(), []( const Outcome& o ) { return o.pass; } );
}

int Report::failed() const
{
	return (int)outcomes_.size() - passed();
}

bool Report::failedAnywhere( const std::string& check ) const
{
	return cellsFailed( check ) > 0;
}

int Report::cellsRun( const std::string& check ) const
{
	return (int)std::count_if( outcomes_.begin(), outcomes_.end(), [ & ]( const Outcome& o ) { return o.check == check; } );
}

int Report::cellsFailed( const std::string& check ) const
{
	return (int)std::count_if( outcomes_.begin(), outcomes_.end(),
							   [ & ]( const Outcome& o ) { return o.check == check && !o.pass; } );
}

void Report::printFigures() const
{
	if( figures_.empty() )
		return;
	std::printf( "\nFigures (worst case across every cell):\n" );
	for( const auto& f : figures_ )
		std::printf( "  %-44s %12.5g\n", f.first.c_str(), f.second.first );
}

void Report::clear()
{
	outcomes_.clear();
	figures_.clear();
}

//---------------------------------------------------------------------------

Driver::Driver( const Run& run, const Config& config, const Mutations& mut ) : run_( run )
{
	engine_.reset( new Engine( config, nullptr ) );
	engine_->mutations() = mut;
}

void Driver::play( const Signal& signal, const FrameFn& onFrame, bool begin )
{
	Engine& e = *engine_;
	if( begin )
	{
		e.feedBegin( signal.fs, signal.channels );
		fed_      = 0;
		frameIdx_ = 0;
		channels_ = signal.channels;
	}
	const size_t frames = signal.frames();
	size_t       pos    = 0;
	std::vector< float > l( 65536 ), r( 65536 );
	while( pos < frames )
	{
		const int n = (int)std::min< size_t >( (size_t)run_.block, frames - pos );
		if( countAllocations )
			startCountingAllocations();
		e.feed( signal.x.data() + pos * (size_t)signal.channels, n );
		if( countAllocations )
			allocations += stopCountingAllocations();
		pos += (size_t)n;
		fed_ += (uint64_t)n;
		const double t = (double)fed_ / run_.fs;
		for( ;; )
		{
			const double vt = (double)( frameIdx_ + 1 ) / run_.fps;
			if( vt > t )
				break;
			++frameIdx_;
			const double t0 = nowMs();
			if( countAllocations )
				startCountingAllocations();
			const Frame& f = e.update( vt, nullptr, 0 );
			int got = 0;
			if( collectTap )
				got = e.readTap( l.data(), r.data(), (int)l.size() );
			if( countAllocations )
				allocations += stopCountingAllocations();
			const double ms = nowMs() - t0;
			maxUpdateMs = std::max( maxUpdateMs, ms );
			totalUpdateMs += ms;
			++updates;
			if( collectTap )
			{
				tapL.insert( tapL.end(), l.begin(), l.begin() + got );
				tapR.insert( tapR.end(), r.begin(), r.begin() + got );
			}
			if( onFrame )
				onFrame( f, vt );
		}
	}
}

//---------------------------------------------------------------------------

const std::vector< Group >& groups()
{
	static const std::vector< Group > g = {
		{ "fft", "the FFT against a direct DFT", checkFft, false },
		{ "pitch", "MPM pitch: sines, missing fundamental, octave traps, vibrato, noise", checkPitch },
		{ "partials", "QIFFT partials and harmonic labels", checkPartials },
		{ "onset-prime", "onsets primed on the first frame (the deaf-for-1.5-s bug)", checkOnsetPrime },
		{ "onset-attack", "soft and hard attacks, the masking gate, latency", checkOnsetAttack },
		{ "bands", "band levels (Parseval) and envelope timing", checkBands },
		{ "thirds", "third-octave bank against the IEC 61260 class 1 masks", checkThirds },
		{ "tempo", "tempo on grooves, at 24/30/60 fps", checkTempo },
		{ "stereo", "correlation, width, balance, pair selection", checkStereo },
		{ "tap", "waveform tap: contiguity, passband, aliasing", checkTap },
		{ "silent", "the Silent detector", checkSilent },
		{ "ring", "the ring under a racing producer and a lapped reader", checkRing, false },
		{ "params", "a parameter change does not clear the ring", checkParams },
		{ "fallback", "the host-FFT fallback", checkFallback },
		{ "capture", "the device worker: slow open, missing, lost, reopen", checkCapture, false },
		{ "alloc", "no allocation on the render or audio thread after warm-up", checkAlloc },
	};
	return g;
}

} // namespace estest

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

/**
	The capture ring: one producer (the audio thread), readers that address
	frames by absolute index.

	Lifted from spasis's `Ring` and changed in one structural way. spasis's
	producer shoves a shared read cursor out of its way when the ring is full;
	here the producer never touches anything a reader owns. It publishes a
	monotonic write count, and each reader keeps its own absolute position and
	asks for frames [start, start + n). A request for frames that have scrolled
	out is refused, so a reader that fell behind finds out and can skip ahead
	instead of analysing audio that was overwritten under it.

	What survives unchanged from spasis, because it was learned the hard way:

	- **Overrun drops the oldest, never the newest.** A render thread that
	  stalls for a second has nothing useful in second-old audio; a ring that
	  refused new frames would lag further behind the music the longer it ran.
	- **Never blocks, never allocates, never locks** on the producer side.
	- **A parameter change must not clear it.** Only a new stream (a device
	  opened, or `feedBegin`) resets it, and readers see that as a new epoch.

	Torn reads. The producer can lap a slow reader in the middle of a copy. So
	the producer claims the frames it is about to write before writing them,
	and a reader re-checks the claim after copying: if the claim reached into
	the frames it copied, the copy is refused. The samples are stored as
	relaxed atomics, which compile to plain loads and stores on every target
	here, so this seqlock is free of data races in the C++ sense rather than
	merely unlikely to tear (Boehm, "Can seqlocks get along with programming
	language memory models?", 2012). `estest --ring` races a real producer
	thread against readers and requires every accepted copy to be intact.
*/
namespace earshot
{

class Ring
{
public:
	static_assert( std::atomic< float >::is_always_lock_free, "relaxed float atomics must be plain moves" );

	/// Construction time only. `frames` is rounded up to a power of two.
	bool allocate( int maxChannels, size_t frames ) noexcept
	{
		size_t cap = 1;
		while( cap < frames )
			cap <<= 1;
		data_.reset( new( std::nothrow ) std::atomic< float >[ cap * (size_t)maxChannels ] );
		if( !data_ )
			return false;
		for( size_t i = 0; i < cap * (size_t)maxChannels; ++i )
			data_[ i ].store( 0.0f, std::memory_order_relaxed );
		capacity_    = cap;
		mask_        = cap - 1;
		maxChannels_ = maxChannels;
		return true;
	}

	size_t capacity() const noexcept { return capacity_; }
	int    maxChannels() const noexcept { return maxChannels_; }

	//-- Producer -------------------------------------------------------------

	/// Start a new stream. Only while no write() can run concurrently.
	void reset( int channels ) noexcept
	{
		const uint32_t e = epoch_.load( std::memory_order_relaxed );
		epoch_.store( e + 1, std::memory_order_relaxed );   // odd: resetting
		std::atomic_thread_fence( std::memory_order_release );
		claim_.store( 0, std::memory_order_relaxed );
		written_.store( 0, std::memory_order_relaxed );
		channels_.store( channels < 1 ? 1 : ( channels > maxChannels_ ? maxChannels_ : channels ),
						 std::memory_order_relaxed );
		epoch_.store( e + 2, std::memory_order_release );   // even: stable
	}

	/// Audio thread. `interleaved` carries channels() channels per frame.
	void write( const float* interleaved, size_t frames ) noexcept
	{
		const int ch = channels_.load( std::memory_order_relaxed );
		if( capacity_ == 0 || frames == 0 )
			return;

		// A block longer than the whole ring can only end as the tail of itself.
		const uint64_t w = written_.load( std::memory_order_relaxed );
		uint64_t       at = w;
		if( frames > capacity_ )
		{
			interleaved += ( frames - capacity_ ) * (size_t)ch;
			at += frames - capacity_;
			frames = capacity_;
		}

		claim_.store( at + frames, std::memory_order_relaxed );
		std::atomic_thread_fence( std::memory_order_release );

		for( size_t f = 0; f < frames; ++f )
		{
			std::atomic< float >* slot = &data_[ ( ( at + f ) & mask_ ) * (size_t)ch ];
			for( int c = 0; c < ch; ++c )
				slot[ c ].store( interleaved[ f * (size_t)ch + (size_t)c ], std::memory_order_relaxed );
		}
		written_.store( at + frames, std::memory_order_release );
	}

	//-- Readers --------------------------------------------------------------

	uint32_t epoch() const noexcept { return epoch_.load( std::memory_order_acquire ); }
	uint64_t written() const noexcept { return written_.load( std::memory_order_acquire ); }
	int      channels() const noexcept { return channels_.load( std::memory_order_relaxed ); }

	/**
		Copy frames [start, start + frames) of epoch `expectEpoch` into `out`,
		interleaved at channels(). False if they are not all written yet, have
		scrolled out, were overwritten during the copy, or the stream restarted.
	*/
	bool read( uint64_t start, size_t frames, float* out, uint32_t expectEpoch, bool validate = true,
			   bool checkEpoch = true ) const noexcept
	{
		// `validate` and `checkEpoch` are false only for the harness's negative
		// controls (ringNoValidate, ringNoEpoch).
		const uint32_t e1 = epoch_.load( std::memory_order_acquire );
		if( ( e1 & 1u ) != 0 || ( checkEpoch && e1 != expectEpoch ) || frames == 0 || frames > capacity_ )
			return false;
		const int      ch = channels_.load( std::memory_order_relaxed );
		const uint64_t w  = written_.load( std::memory_order_acquire );
		if( start + frames > w || w - start > capacity_ )
			return false;

		for( size_t f = 0; f < frames; ++f )
		{
			const std::atomic< float >* slot = &data_[ ( ( start + f ) & mask_ ) * (size_t)ch ];
			for( int c = 0; c < ch; ++c )
				out[ f * (size_t)ch + (size_t)c ] = slot[ c ].load( std::memory_order_relaxed );
		}

		if( !validate )
			return true;
		std::atomic_thread_fence( std::memory_order_acquire );
		const uint64_t claim = claim_.load( std::memory_order_relaxed );
		const uint32_t e2    = epoch_.load( std::memory_order_relaxed );
		return e2 == e1 && claim >= start && claim - start <= capacity_;
	}

private:
	std::unique_ptr< std::atomic< float >[] > data_;
	size_t capacity_    = 0;
	size_t mask_        = 0;
	int    maxChannels_ = 0;

	std::atomic< int >      channels_{ 1 };
	std::atomic< uint32_t > epoch_{ 0 };
	std::atomic< uint64_t > claim_{ 0 };
	std::atomic< uint64_t > written_{ 0 };
};

} // namespace earshot

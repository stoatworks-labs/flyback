#include "Wav.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace estest
{
namespace
{
uint32_t u32( const unsigned char* p ) { return (uint32_t)p[ 0 ] | (uint32_t)p[ 1 ] << 8 | (uint32_t)p[ 2 ] << 16 | (uint32_t)p[ 3 ] << 24; }
uint16_t u16( const unsigned char* p ) { return (uint16_t)( p[ 0 ] | p[ 1 ] << 8 ); }

void put32( std::vector< unsigned char >& b, uint32_t v )
{
	for( int i = 0; i < 4; ++i )
		b.push_back( (unsigned char)( v >> ( 8 * i ) ) );
}
void put16( std::vector< unsigned char >& b, uint16_t v )
{
	b.push_back( (unsigned char)v );
	b.push_back( (unsigned char)( v >> 8 ) );
}
} // namespace

bool readWav( const std::string& path, Signal& out, std::string& error )
{
	FILE* f = std::fopen( path.c_str(), "rb" );
	if( f == nullptr )
	{
		error = "cannot open " + path;
		return false;
	}
	std::vector< unsigned char > data;
	unsigned char                buf[ 65536 ];
	size_t                       n;
	while( ( n = std::fread( buf, 1, sizeof( buf ), f ) ) > 0 )
		data.insert( data.end(), buf, buf + n );
	std::fclose( f );
	if( data.size() < 12 || std::memcmp( data.data(), "RIFF", 4 ) != 0 || std::memcmp( data.data() + 8, "WAVE", 4 ) != 0 )
	{
		error = "not a RIFF WAVE file";
		return false;
	}
	int    format = 0, channels = 0, rate = 0, bits = 0;
	size_t pos = 12, dataAt = 0, dataLen = 0;
	while( pos + 8 <= data.size() )
	{
		const uint32_t len = u32( &data[ pos + 4 ] );
		const unsigned char* body = &data[ pos + 8 ];
		if( std::memcmp( &data[ pos ], "fmt ", 4 ) == 0 && len >= 16 )
		{
			format   = u16( body );
			channels = u16( body + 2 );
			rate     = (int)u32( body + 4 );
			bits     = u16( body + 14 );
			if( format == 0xFFFE && len >= 26 )
				format = u16( body + 24 );   // the sub-format GUID's first two bytes
		}
		else if( std::memcmp( &data[ pos ], "data", 4 ) == 0 )
		{
			dataAt  = pos + 8;
			dataLen = std::min< size_t >( len, data.size() - dataAt );
		}
		pos += 8 + (size_t)len + ( len & 1u );
	}
	if( channels <= 0 || rate <= 0 || dataAt == 0 )
	{
		error = "no fmt or data chunk";
		return false;
	}
	const int bytes = bits / 8;
	if( !( ( format == 1 && ( bits == 16 || bits == 24 || bits == 32 ) ) || ( format == 3 && ( bits == 32 || bits == 64 ) ) ) )
	{
		error = "unsupported sample format";
		return false;
	}
	const size_t samples = dataLen / (size_t)bytes;
	out.fs       = rate;
	out.channels = channels;
	out.x.resize( samples - samples % (size_t)channels );
	for( size_t i = 0; i < out.x.size(); ++i )
	{
		const unsigned char* p = &data[ dataAt + i * (size_t)bytes ];
		float                v = 0.0f;
		if( format == 3 && bits == 32 )
			std::memcpy( &v, p, 4 );
		else if( format == 3 )
		{
			double d;
			std::memcpy( &d, p, 8 );
			v = (float)d;
		}
		else if( bits == 16 )
			v = (float)(int16_t)u16( p ) / 32768.0f;
		else if( bits == 24 )
			v = (float)( (int32_t)( (uint32_t)p[ 0 ] << 8 | (uint32_t)p[ 1 ] << 16 | (uint32_t)p[ 2 ] << 24 ) >> 8 ) / 8388608.0f;
		else
			v = (float)( (double)(int32_t)u32( p ) / 2147483648.0 );
		out.x[ i ] = v;
	}
	return true;
}

bool writeWav( const std::string& path, const Signal& in, std::string& error )
{
	std::vector< unsigned char > b;
	const uint32_t               dataLen = (uint32_t)( in.x.size() * 4 );
	b.insert( b.end(), { 'R', 'I', 'F', 'F' } );
	put32( b, 36 + dataLen );
	b.insert( b.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' } );
	put32( b, 16 );
	put16( b, 3 );
	put16( b, (uint16_t)in.channels );
	put32( b, (uint32_t)in.fs );
	put32( b, (uint32_t)( in.fs * in.channels * 4 ) );
	put16( b, (uint16_t)( in.channels * 4 ) );
	put16( b, 32 );
	b.insert( b.end(), { 'd', 'a', 't', 'a' } );
	put32( b, dataLen );
	const size_t at = b.size();
	b.resize( at + dataLen );
	std::memcpy( b.data() + at, in.x.data(), dataLen );
	FILE* f = std::fopen( path.c_str(), "wb" );
	if( f == nullptr || std::fwrite( b.data(), 1, b.size(), f ) != b.size() )
	{
		if( f != nullptr )
			std::fclose( f );
		error = "cannot write " + path;
		return false;
	}
	std::fclose( f );
	return true;
}

} // namespace estest

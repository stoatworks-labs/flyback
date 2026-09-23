#include "Lattice.h"

#include <algorithm>
#include <cmath>

namespace flyback
{
namespace
{
/// Gauss-Seidel sweeps on the coarsest level, which is a few dozen cells.
constexpr int kCoarsestSweeps = 40;
} // namespace

int Lattice::GoodSize( int wanted, int levelsWanted )
{
	const int step = 1 << levelsWanted;
	const int a    = std::max( 2, ( std::max( wanted, 3 ) - 1 + step - 1 ) / step );
	return a * step + 1;
}

bool Lattice::Resize( int nx_, int ny_ )
{
	if( nx_ < 5 || ny_ < 5 || ( nx_ - 1 ) % 4 != 0 || ( ny_ - 1 ) % 4 != 0 )
		return false;

	nx = nx_;
	ny = ny_;
	const size_t n = static_cast< size_t >( nx ) * static_cast< size_t >( ny );
	phi.assign( n, 0.0f );
	held.assign( n, 0 );
	r.assign( n, 0.0f );
	z.assign( n, 0.0f );
	p.assign( n, 0.0f );
	ap.assign( n, 0.0f );

	levels.clear();
	int lx = nx, ly = ny;
	while( true )
	{
		Level level;
		level.nx = lx;
		level.ny = ly;
		const size_t m = static_cast< size_t >( lx ) * static_cast< size_t >( ly );
		level.u.assign( m, 0.0f );
		level.f.assign( m, 0.0f );
		level.r.assign( m, 0.0f );
		level.held.assign( m, 0 );
		level.free.assign( m, 1.0f );
		levels.push_back( std::move( level ) );

		if( ( lx - 1 ) % 2 != 0 || ( ly - 1 ) % 2 != 0 || std::min( lx, ly ) < 9 )
			break;
		lx = ( lx - 1 ) / 2 + 1;
		ly = ( ly - 1 ) / 2 + 1;
	}

	// The outer ring is always held. The caller sets what it is held at.
	for( int i = 0; i < nx; ++i )
	{
		held[ static_cast< size_t >( Index( i, 0 ) ) ]      = 1;
		held[ static_cast< size_t >( Index( i, ny - 1 ) ) ] = 1;
	}
	for( int j = 0; j < ny; ++j )
	{
		held[ static_cast< size_t >( Index( 0, j ) ) ]      = 1;
		held[ static_cast< size_t >( Index( nx - 1, j ) ) ] = 1;
	}
	masksDirty = true;
	return true;
}

void Lattice::Hold( int index, float value )
{
	const size_t k = static_cast< size_t >( index );
	phi[ k ]       = value;
	if( held[ k ] )
		return;
	held[ k ] = 1;
	if( !masksDirty )
	{
		levels[ 0 ].held[ k ] = 1;
		levels[ 0 ].free[ k ] = 0.0f;
		MarkCoarse( 1, index % nx, index / nx );
	}
}

void Lattice::Release( int index )
{
	const size_t k = static_cast< size_t >( index );
	const int i    = index % nx;
	const int j    = index / nx;
	if( i == 0 || j == 0 || i == nx - 1 || j == ny - 1 )
		return;// the ring stays held
	if( held[ k ] )
	{
		held[ k ]  = 0;
		masksDirty = true;
	}
}

void Lattice::ReleaseAll( float guess )
{
	for( int j = 1; j < ny - 1; ++j )
		for( int i = 1; i < nx - 1; ++i )
		{
			const size_t k = static_cast< size_t >( Index( i, j ) );
			held[ k ]      = 0;
			phi[ k ]       = guess;
		}
	masksDirty = true;
}

void Lattice::Guess( int index, float value )
{
	const size_t k = static_cast< size_t >( index );
	if( !held[ k ] )
		phi[ k ] = value;
}

//---------------------------------------------------------------------------
// Masks. A coarse cell is held when any fine cell under its 3x3 stencil is.
//---------------------------------------------------------------------------
void Lattice::MarkCoarse( int level, int i, int j )
{
	if( level >= static_cast< int >( levels.size() ) )
		return;
	Level& c = levels[ static_cast< size_t >( level ) ];
	// Coarse X covers fine 2X-1 .. 2X+1, so fine i is under X = floor(i/2)
	// and X = ceil(i/2).
	const int x0 = i / 2, x1 = ( i + 1 ) / 2;
	const int y0 = j / 2, y1 = ( j + 1 ) / 2;
	for( int y = y0; y <= y1; ++y )
		for( int x = x0; x <= x1; ++x )
		{
			if( x < 0 || y < 0 || x >= c.nx || y >= c.ny )
				continue;
			uint8_t& h = c.held[ static_cast< size_t >( y * c.nx + x ) ];
			if( h )
				continue;
			h                                           = 1;
			c.free[ static_cast< size_t >( y * c.nx + x ) ] = 0.0f;
			MarkCoarse( level + 1, x, y );
		}
}

void Lattice::BuildMasks()
{
	levels[ 0 ].held = held;
	for( size_t k = 1; k < levels.size(); ++k )
	{
		const Level& f = levels[ k - 1 ];
		Level& c       = levels[ k ];
		std::fill( c.held.begin(), c.held.end(), 0 );
		for( int y = 0; y < c.ny; ++y )
			for( int x = 0; x < c.nx; ++x )
			{
				bool any = x == 0 || y == 0 || x == c.nx - 1 || y == c.ny - 1;
				for( int dy = -1; dy <= 1 && !any; ++dy )
					for( int dx = -1; dx <= 1 && !any; ++dx )
					{
						const int fx = 2 * x + dx, fy = 2 * y + dy;
						if( fx >= 0 && fy >= 0 && fx < f.nx && fy < f.ny && f.held[ static_cast< size_t >( fy * f.nx + fx ) ] )
							any = true;
					}
				c.held[ static_cast< size_t >( y * c.nx + x ) ] = any ? 1 : 0;
			}
	}
	for( Level& l : levels )
		for( size_t i = 0; i < l.held.size(); ++i )
			l.free[ i ] = l.held[ i ] ? 0.0f : 1.0f;
	masksDirty = false;
}

//---------------------------------------------------------------------------
// The V-cycle.
//---------------------------------------------------------------------------
void Lattice::Smooth( Level& l, int sweeps, bool redFirst ) const
{
	const int w       = l.nx;
	float* u          = l.u.data();
	const float* f    = l.f.data();
	const float* m    = l.free.data();
	for( int s = 0; s < sweeps; ++s )
		for( int pass = 0; pass < 2; ++pass )
		{
			const int colour = redFirst ? pass : 1 - pass;
			for( int y = 1; y < l.ny - 1; ++y )
			{
				const int x0 = 1 + ( ( y + colour ) & 1 );
				float* row   = u + y * w;
				const float* up = row - w;
				const float* dn = row + w;
				const float* fr = f + y * w;
				const float* mr = m + y * w;
				// Branchless: a held cell's mask is 0, so it keeps its value.
				for( int x = x0; x < w - 1; x += 2 )
					row[ x ] += mr[ x ] * ( 0.25f * ( row[ x - 1 ] + row[ x + 1 ] + up[ x ] + dn[ x ] - fr[ x ] ) - row[ x ] );
			}
		}
}

void Lattice::ResidualOf( Level& l ) const
{
	const int w = l.nx;
	for( int y = 1; y < l.ny - 1; ++y )
	{
		const float* u  = l.u.data() + y * w;
		const float* f  = l.f.data() + y * w;
		const float* m  = l.free.data() + y * w;
		float* r        = l.r.data() + y * w;
		for( int x = 1; x < w - 1; ++x )
			r[ x ] = m[ x ] * ( f[ x ] - ( u[ x - 1 ] + u[ x + 1 ] + u[ x - w ] + u[ x + w ] - 4.0f * u[ x ] ) );
	}
}

void Lattice::Restrict( const Level& fine, Level& coarse ) const
{
	const int fw = fine.nx, cw = coarse.nx;
	std::fill( coarse.u.begin(), coarse.u.end(), 0.0f );
	std::fill( coarse.f.begin(), coarse.f.end(), 0.0f );
	for( int y = 1; y < coarse.ny - 1; ++y )
		for( int x = 1; x < cw - 1; ++x )
		{
			const int c = ( 2 * y ) * fw + 2 * x;
			const float* rr = fine.r.data();
			const float v = 0.25f * rr[ c ]
			              + 0.125f * ( rr[ c - 1 ] + rr[ c + 1 ] + rr[ c - fw ] + rr[ c + fw ] )
			              + 0.0625f * ( rr[ c - fw - 1 ] + rr[ c - fw + 1 ] + rr[ c + fw - 1 ] + rr[ c + fw + 1 ] );
			// The coarse equation is written with unit spacing too, and the
			// five-point Laplacian at spacing 2 is four times the one at 1.
			coarse.f[ static_cast< size_t >( y * cw + x ) ] = 4.0f * v;
		}
}

void Lattice::ProlongAdd( const Level& coarse, Level& fine )
{
	const int fw = fine.nx, cw = coarse.nx;
	const float* cu = coarse.u.data();
	rowScratch.resize( static_cast< size_t >( cw ) );
	float* a = rowScratch.data();
	for( int y = 1; y < fine.ny - 1; ++y )
	{
		// Bilinear, separably: first down the coarse rows...
		const int Y     = y >> 1;
		const float* c0 = cu + Y * cw;
		if( y & 1 )
		{
			const float* c1 = c0 + cw;
			for( int X = 0; X < cw; ++X )
				a[ X ] = 0.5f * ( c0[ X ] + c1[ X ] );
		}
		else
			std::copy( c0, c0 + cw, a );
		// ...then across. A held cell's mask is 0, so it keeps its value.
		float* u       = fine.u.data() + y * fw;
		const float* m = fine.free.data() + y * fw;
		for( int x = 1; x < fw - 1; ++x )
		{
			const int X   = x >> 1;
			const float v = ( x & 1 ) ? 0.5f * ( a[ X ] + a[ X + 1 ] ) : a[ X ];
			u[ x ] += m[ x ] * v;
		}
	}
}

void Lattice::VCycle( size_t k )
{
	Level& l = levels[ k ];
	if( k + 1 == levels.size() )
	{
		// Symmetric on the coarsest level too: red-black forward, then back.
		Smooth( l, kCoarsestSweeps / 2, true );
		Smooth( l, kCoarsestSweeps / 2, false );
		return;
	}
	Smooth( l, 2, true );
	ResidualOf( l );
	Restrict( l, levels[ k + 1 ] );
	VCycle( k + 1 );
	ProlongAdd( levels[ k + 1 ], l );
	Smooth( l, 2, false );
}

void Lattice::Precondition( const std::vector< float >& in, std::vector< float >& out )
{
	// z ~ A^-1 in, where A = -L on the free cells: one V-cycle on L e = -in
	// from a zero start. `in` is already zero on held cells, and nothing in
	// the V-cycle writes a held cell, so neither needs a mask here.
	Level& l       = levels[ 0 ];
	const size_t n = in.size();
	std::fill( l.u.begin(), l.u.end(), 0.0f );
	for( size_t i = 0; i < n; ++i )
		l.f[ i ] = -in[ i ];
	VCycle( 0 );
	std::copy( l.u.begin(), l.u.end(), out.begin() );
}

void Lattice::ApplyA( const std::vector< float >& x, std::vector< float >& y ) const
{
	const float* m = levels[ 0 ].free.data();
	for( int j = 1; j < ny - 1; ++j )
	{
		const float* xr = x.data() + j * nx;
		float* yr       = y.data() + j * nx;
		const float* mr = m + j * nx;
		for( int i = 1; i < nx - 1; ++i )
			yr[ i ] = -mr[ i ] * ( xr[ i - 1 ] + xr[ i + 1 ] + xr[ i - nx ] + xr[ i + nx ] - 4.0f * xr[ i ] );
	}
}

//---------------------------------------------------------------------------
// PCG.
//---------------------------------------------------------------------------
int Lattice::Solve( float tolerance, int maxIterations )
{
	if( levels.empty() )
		return -1;
	if( masksDirty )
		BuildMasks();

	++solves;

	// rho = b - A phi, which for Laplace's equation is just L phi on the free
	// cells, the held values standing in for b.
	const size_t n = phi.size();
	const float* m = levels[ 0 ].free.data();
	float worst    = 0.0f;
	std::fill( r.begin(), r.end(), 0.0f );
	for( int j = 1; j < ny - 1; ++j )
	{
		const float* pr = phi.data() + j * nx;
		float* rr       = r.data() + j * nx;
		const float* mr = m + j * nx;
		for( int i = 1; i < nx - 1; ++i )
		{
			rr[ i ] = mr[ i ] * ( pr[ i - 1 ] + pr[ i + 1 ] + pr[ i - nx ] + pr[ i + nx ] - 4.0f * pr[ i ] );
			worst   = std::max( worst, std::fabs( rr[ i ] ) );
		}
	}
	if( worst < tolerance )
		return 0;

	Precondition( r, z );
	p         = z;
	double rz = 0.0;
	for( size_t k = 0; k < n; ++k )
		rz += static_cast< double >( r[ k ] ) * z[ k ];

	for( int it = 1; it <= maxIterations; ++it )
	{
		ApplyA( p, ap );
		double pap = 0.0;
		for( size_t k = 0; k < n; ++k )
			pap += static_cast< double >( p[ k ] ) * ap[ k ];
		if( pap <= 0.0 )
			return -1;
		const float alpha = static_cast< float >( rz / pap );

		// p and A p are zero on every held cell, so these need no mask.
		worst = 0.0f;
		for( size_t k = 0; k < n; ++k )
		{
			phi[ k ] += alpha * p[ k ];
			r[ k ] -= alpha * ap[ k ];
			worst = std::max( worst, std::fabs( r[ k ] ) );
		}
		++iterations;
		if( worst < tolerance )
			return it;

		Precondition( r, z );
		double rzNext = 0.0;
		for( size_t k = 0; k < n; ++k )
			rzNext += static_cast< double >( r[ k ] ) * z[ k ];
		const float beta = static_cast< float >( rzNext / rz );
		rz               = rzNext;
		for( size_t k = 0; k < n; ++k )
			p[ k ] = z[ k ] + beta * p[ k ];
	}
	return -1;
}

void Lattice::Relax( int ci, int cj, int radius, int sweeps )
{
	const int x0 = std::max( 1, ci - radius ), x1 = std::min( nx - 2, ci + radius );
	const int y0 = std::max( 1, cj - radius ), y1 = std::min( ny - 2, cj + radius );
	for( int s = 0; s < sweeps; ++s )
		for( int j = y0; j <= y1; ++j )
			for( int i = x0; i <= x1; ++i )
			{
				const size_t k = static_cast< size_t >( j * nx + i );
				if( held[ k ] )
					continue;
				phi[ k ] = 0.25f * ( phi[ k - 1 ] + phi[ k + 1 ] + phi[ k - static_cast< size_t >( nx ) ]
				                     + phi[ k + static_cast< size_t >( nx ) ] );
			}
}

float Lattice::Residual() const
{
	float worst = 0.0f;
	for( int j = 1; j < ny - 1; ++j )
		for( int i = 1; i < nx - 1; ++i )
		{
			const size_t k = static_cast< size_t >( j * nx + i );
			if( held[ k ] )
				continue;
			worst = std::max( worst, std::fabs( phi[ k - 1 ] + phi[ k + 1 ] + phi[ k - static_cast< size_t >( nx ) ]
			                                    + phi[ k + static_cast< size_t >( nx ) ] - 4.0f * phi[ k ] ) );
		}
	return worst;
}
} // namespace flyback

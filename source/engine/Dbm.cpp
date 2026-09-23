#include "Dbm.h"

#include <algorithm>
#include <cmath>

namespace flyback
{
namespace
{
/// Neighbours: the four bonds first, then the four diagonals, in a fixed
/// order so the choice of parent never depends on anything but the lattice.
constexpr int kDx[ 8 ] = { 1, -1, 0, 0, 1, -1, 1, -1 };
constexpr int kDy[ 8 ] = { 0, 0, 1, -1, 1, 1, -1, -1 };
constexpr double kBond[ 8 ] = { 1.0, 1.0, 1.0, 1.0, 1.4142135623730951, 1.4142135623730951, 1.4142135623730951,
	                            1.4142135623730951 };
} // namespace

bool Field::Reset( double widthMetres, double heightMetres, int cellsHigh, float ringPotential )
{
	cellsHigh = std::max( cellsHigh, 16 );
	h         = heightMetres / cellsHigh;
	// Four levels of coarsening: the lattice grows to the next multiple of 16,
	// plus one, and the extra is split evenly round the scene.
	const int ny = Lattice::GoodSize( cellsHigh + 1, 4 );
	const int nx = Lattice::GoodSize( static_cast< int >( std::ceil( widthMetres / h ) ) + 1, 4 );
	if( !lattice.Resize( nx, ny ) )
		return false;

	x0 = -0.5 * h * ( nx - 1 );
	y0 = -0.5 * h * ( ny - 1 );
	const size_t n = static_cast< size_t >( nx ) * static_cast< size_t >( ny );
	kind.assign( n, Cell::Free );
	nodeAt.assign( n, -1 );
	slotOf.assign( n, -1 );
	for( int i = 0; i < nx; ++i )
		for( int j : { 0, ny - 1 } )
		{
			kind[ static_cast< size_t >( lattice.Index( i, j ) ) ] = Cell::Wall;
			lattice.Hold( lattice.Index( i, j ), ringPotential );
		}
	for( int j = 0; j < ny; ++j )
		for( int i : { 0, nx - 1 } )
		{
			kind[ static_cast< size_t >( lattice.Index( i, j ) ) ] = Cell::Wall;
			lattice.Hold( lattice.Index( i, j ), ringPotential );
		}
	GuessAll( ringPotential );
	return true;
}

int Field::CellAt( double x, double y ) const
{
	const int i = static_cast< int >( std::lround( ( x - x0 ) / h ) );
	const int j = static_cast< int >( std::lround( ( y - y0 ) / h ) );
	if( i < 1 || j < 1 || i > lattice.Nx() - 2 || j > lattice.Ny() - 2 )
		return -1;
	return lattice.Index( i, j );
}

void Field::PaintSource( int index )
{
	if( index < 0 || kind[ static_cast< size_t >( index ) ] == Cell::Channel || kind[ static_cast< size_t >( index ) ] == Cell::Arc
	    || kind[ static_cast< size_t >( index ) ] == Cell::Wall )
		return;
	kind[ static_cast< size_t >( index ) ] = Cell::Source;
	lattice.Hold( index, 1.0f );
}

void Field::PaintGround( int index, float potential )
{
	if( index < 0 || kind[ static_cast< size_t >( index ) ] == Cell::Channel || kind[ static_cast< size_t >( index ) ] == Cell::Arc
	    || kind[ static_cast< size_t >( index ) ] == Cell::Wall )
		return;
	kind[ static_cast< size_t >( index ) ] = Cell::Ground;
	lattice.Hold( index, potential );
}

void Field::PaintHeld( int index, float potential )
{
	if( index < 0 || kind[ static_cast< size_t >( index ) ] == Cell::Channel || kind[ static_cast< size_t >( index ) ] == Cell::Arc
	    || kind[ static_cast< size_t >( index ) ] == Cell::Wall )
		return;
	kind[ static_cast< size_t >( index ) ] = Cell::Held;
	lattice.Hold( index, potential );
}

void Field::ClearConductors()
{
	for( size_t k = 0; k < kind.size(); ++k )
		if( kind[ k ] != Cell::Wall )
		{
			kind[ k ]   = Cell::Free;
			nodeAt[ k ] = -1;
			lattice.Release( static_cast< int >( k ) );
		}
}

void Field::Adopt( Tree& tree )
{
	std::fill( nodeAt.begin(), nodeAt.end(), -1 );
	for( size_t k = 0; k < kind.size(); ++k )
		if( kind[ k ] == Cell::Channel || kind[ k ] == Cell::Arc )
		{
			kind[ k ] = Cell::Free;
			lattice.Release( static_cast< int >( k ) );
		}

	std::vector< Node >& nodes = tree.Nodes();
	for( size_t n = 0; n < nodes.size(); ++n )
	{
		Node& node = nodes[ n ];
		if( !node.alive )
			continue;
		if( node.parent >= 0 && !nodes[ static_cast< size_t >( node.parent ) ].alive )
		{
			node.alive = false;
			continue;
		}
		const int cell = node.cell;
		if( cell < 0 || kind[ static_cast< size_t >( cell ) ] != Cell::Free )
		{
			node.alive = false;
			continue;
		}
		kind[ static_cast< size_t >( cell ) ]   = Cell::Channel;
		nodeAt[ static_cast< size_t >( cell ) ] = static_cast< int >( n );
		lattice.Hold( cell, 1.0f );
	}
}

void Field::Forget( const Node& node )
{
	if( node.cell < 0
	    || ( kind[ static_cast< size_t >( node.cell ) ] != Cell::Channel && kind[ static_cast< size_t >( node.cell ) ] != Cell::Arc ) )
		return;
	kind[ static_cast< size_t >( node.cell ) ]   = Cell::Free;
	nodeAt[ static_cast< size_t >( node.cell ) ] = -1;
	lattice.Release( node.cell );
}

void Field::Conduct( Tree& tree, int tip, float endPotential )
{
	std::vector< Node >& nodes = tree.Nodes();
	std::vector< int > path;
	for( int n = tip; n >= 0; n = nodes[ static_cast< size_t >( n ) ].parent )
		path.push_back( n );
	// Path lengths from the root, then the linear fall.
	std::vector< double > along( path.size(), 0.0 );
	double total = 0.0;
	for( size_t k = path.size(); k-- > 0; )
	{
		const Node& node = nodes[ static_cast< size_t >( path[ k ] ) ];
		const float px   = node.parent >= 0 ? nodes[ static_cast< size_t >( node.parent ) ].x : node.ax;
		const float py   = node.parent >= 0 ? nodes[ static_cast< size_t >( node.parent ) ].y : node.ay;
		total += std::hypot( node.x - px, node.y - py );
		along[ k ] = total;
	}
	const Node& end = nodes[ static_cast< size_t >( tip ) ];
	total += std::hypot( end.gx - end.x, end.gy - end.y );
	for( size_t k = 0; k < path.size(); ++k )
	{
		const int cell = nodes[ static_cast< size_t >( path[ k ] ) ].cell;
		if( cell < 0 )
			continue;
		const double f = total > 0.0 ? along[ k ] / total : 1.0;
		kind[ static_cast< size_t >( cell ) ] = Cell::Arc;
		lattice.Hold( cell, static_cast< float >( 1.0 - ( 1.0 - endPotential ) * f ) );
	}
}

void Field::GuessAll( float value )
{
	for( size_t k = 0; k < kind.size(); ++k )
		if( kind[ k ] == Cell::Free )
			lattice.Guess( static_cast< int >( k ), value );
}

//---------------------------------------------------------------------------
// Candidates.
//---------------------------------------------------------------------------
double Field::WeightOf( int cell, const GrowthSettings& settings, int& parentCell ) const
{
	const int nx = lattice.Nx();
	const int i = cell % nx, j = cell / nx;
	parentCell = -1;
	int bestK  = -1;
	const int ways = settings.diagonal ? 8 : 4;
	for( int k = 0; k < ways; ++k )
	{
		const int ni = i + kDx[ k ], nj = j + kDy[ k ];
		const Cell c = kind[ static_cast< size_t >( lattice.Index( ni, nj ) ) ];
		if( c != Cell::Channel && c != Cell::Source )
			continue;
		// The shortest bond carries the strongest field; among equals, grow
		// the channel rather than start a new one on the electrode.
		if( bestK < 0 || kBond[ k ] < kBond[ bestK ]
		    || ( kBond[ k ] == kBond[ bestK ] && c == Cell::Channel
		         && kind[ static_cast< size_t >( parentCell ) ] == Cell::Source ) )
		{
			bestK      = k;
			parentCell = lattice.Index( ni, nj );
		}
	}
	if( bestK < 0 )
		return 0.0;

	const double phi   = std::clamp( static_cast< double >( lattice.Potential( cell ) ), 0.0, 1.0 );
	const double field = ( 1.0 - phi ) / kBond[ bestK ];
	if( field <= 0.0 )
		return 0.0;
	double w = settings.eta == 0.0 ? 1.0 : std::pow( field, settings.eta );
	if( settings.disorder > 0.0f )
	{
		// A standard normal from two hashed uniforms (Box-Muller), fixed for
		// this site and this burst.
		const uint32_t h1 = Hash( static_cast< uint32_t >( cell ), settings.disorderKey );
		const uint32_t h2 = Hash( h1 ^ 0x68e31da4U );
		const double u1   = ( ( h1 >> 8 ) + 0.5 ) * ( 1.0 / 16777216.0 );
		const double u2   = ( h2 >> 8 ) * ( 1.0 / 16777216.0 );
		const double xi   = std::sqrt( -2.0 * std::log( u1 ) ) * std::cos( 6.283185307179586 * u2 );
		w *= std::exp( settings.disorder * xi );
	}
	if( settings.biasX != 0.0f || settings.biasY != 0.0f )
		w *= std::exp( settings.biasX * kDx[ bestK ] + settings.biasY * kDy[ bestK ] );
	return w;
}

void Field::SetWeight( int slot, double w )
{
	const double delta = w - weight[ static_cast< size_t >( slot ) ];
	weight[ static_cast< size_t >( slot ) ] = w;
	for( size_t k = static_cast< size_t >( slot ) + 1; k < fenwick.size(); k += k & ( ~k + 1 ) )
		fenwick[ k ] += delta;
}

int Field::Pick( double u ) const
{
	// Fenwick descent: the first slot whose prefix sum exceeds u.
	size_t pos  = 0;
	size_t step = 1;
	while( step * 2 < fenwick.size() )
		step *= 2;
	for( ; step > 0; step /= 2 )
	{
		const size_t next = pos + step;
		if( next < fenwick.size() && fenwick[ next ] <= u )
		{
			pos = next;
			u -= fenwick[ next ];
		}
	}
	// pos is the count of slots whose cumulative weight is <= u.
	size_t slot = std::min( pos, weight.size() - 1 );
	// Rounding in the tree can land on an empty slot; walk to a live one.
	for( size_t tries = 0; tries < weight.size(); ++tries )
	{
		if( cellOf[ slot ] >= 0 && weight[ slot ] > 0.0 )
			return static_cast< int >( slot );
		slot = slot == 0 ? weight.size() - 1 : slot - 1;
	}
	return -1;
}

void Field::AddCandidate( int cell )
{
	if( slotOf[ static_cast< size_t >( cell ) ] >= 0 )
		return;
	int slot;
	if( !freeSlots.empty() )
	{
		slot = freeSlots.back();
		freeSlots.pop_back();
	}
	else
	{
		slot = static_cast< int >( cellOf.size() );
		cellOf.push_back( -1 );
		weight.push_back( 0.0 );
		if( fenwick.size() < cellOf.size() + 1 )
		{
			// Grow the tree to the next power of two and rebuild it.
			size_t capacity = 1;
			while( capacity < cellOf.size() + 1 )
				capacity *= 2;
			fenwick.assign( capacity + 1, 0.0 );
			weight.resize( capacity, 0.0 );
			cellOf.resize( capacity, -1 );
			for( size_t s = capacity; s-- > static_cast< size_t >( slot ) + 1; )
				freeSlots.push_back( static_cast< int >( s ) );
			for( size_t s = 0; s < capacity; ++s )
				if( weight[ s ] != 0.0 )
					for( size_t k = s + 1; k < fenwick.size(); k += k & ( ~k + 1 ) )
						fenwick[ k ] += weight[ s ];
		}
	}
	cellOf[ static_cast< size_t >( slot ) ]  = cell;
	slotOf[ static_cast< size_t >( cell ) ] = slot;
}

void Field::RemoveCandidate( int cell )
{
	const int slot = slotOf[ static_cast< size_t >( cell ) ];
	if( slot < 0 )
		return;
	SetWeight( slot, 0.0 );
	cellOf[ static_cast< size_t >( slot ) ]  = -1;
	slotOf[ static_cast< size_t >( cell ) ] = -1;
	freeSlots.push_back( slot );
}

void Field::Rebuild( const GrowthSettings& settings )
{
	for( int cell : cellOf )
		if( cell >= 0 )
			slotOf[ static_cast< size_t >( cell ) ] = -1;
	cellOf.clear();
	weight.clear();
	fenwick.assign( 1, 0.0 );
	freeSlots.clear();

	const int nx = lattice.Nx(), ny = lattice.Ny();
	for( int j = 1; j < ny - 1; ++j )
		for( int i = 1; i < nx - 1; ++i )
		{
			const int cell = lattice.Index( i, j );
			if( kind[ static_cast< size_t >( cell ) ] != Cell::Free )
				continue;
			if( settings.allowed != nullptr && !( *settings.allowed )[ static_cast< size_t >( cell ) ] )
				continue;
			for( int k = 0; k < ( settings.diagonal ? 8 : 4 ); ++k )
			{
				const Cell c = kind[ static_cast< size_t >( lattice.Index( i + kDx[ k ], j + kDy[ k ] ) ) ];
				if( c == Cell::Channel || c == Cell::Source )
				{
					AddCandidate( cell );
					break;
				}
			}
		}

	// Weights in one pass, and the tree built from them in O(n).
	std::fill( fenwick.begin(), fenwick.end(), 0.0 );
	for( size_t s = 0; s < cellOf.size(); ++s )
	{
		int parent = -1;
		weight[ s ] = cellOf[ s ] >= 0 ? WeightOf( cellOf[ s ], settings, parent ) : 0.0;
	}
	for( size_t s = 0; s < weight.size() && s + 1 < fenwick.size(); ++s )
	{
		fenwick[ s + 1 ] += weight[ s ];
		const size_t up = ( s + 1 ) + ( ( s + 1 ) & ( ~( s + 1 ) + 1 ) );
		if( up < fenwick.size() )
			fenwick[ up ] += fenwick[ s + 1 ];
	}
	lastCandidates = cellOf.size() - freeSlots.size();
}

bool Field::TouchesGround( int cell, int& groundCell ) const
{
	const int nx = lattice.Nx();
	const int i = cell % nx, j = cell / nx;
	for( int k = 0; k < 8; ++k )
	{
		const int n = lattice.Index( i + kDx[ k ], j + kDy[ k ] );
		// The ring is the room's far walls. Held near ground it is ground, and
		// reaching it ends the discharge: without this a spark that found the
		// ring crawled round the whole frame along it, where the field beside a
		// 0 V wall is strongest, and never completed (a 1.6 m Van de Graaff
		// spark, 1000 sites, 86 ms). A ring held at a middling potential -- the
		// ladder's, 0.5 -- is not ground.
		if( kind[ static_cast< size_t >( n ) ] == Cell::Ground
		    || ( kind[ static_cast< size_t >( n ) ] == Cell::Wall && lattice.Potential( n ) <= 0.25f ) )
		{
			groundCell = n;
			return true;
		}
	}
	return false;
}

//---------------------------------------------------------------------------
// Growth.
//---------------------------------------------------------------------------
GrowthResult Field::Grow( Tree& tree, int maxSites, double lengthBudget, Pcg32& rng, double now,
                          const GrowthSettings& settings )
{
	GrowthResult result;
	if( maxSites <= 0 || lengthBudget <= 0.0 )
		return result;

	lattice.Solve( settings.tolerance );
	Rebuild( settings );

	const int nx    = lattice.Nx();
	int sinceSolve  = 0;
	const int reach = settings.relaxRadius;

	while( result.grown < maxSites && result.length < lengthBudget )
	{
		const double total = fenwick.size() > 1 ? [ & ] {
			double s = 0.0;
			for( size_t k = fenwick.size() - 1; k > 0; k -= k & ( ~k + 1 ) )
				s += fenwick[ k ];
			return s;
		}()
		                                         : 0.0;
		if( !( total > 0.0 ) )
			break;

		const int slot = Pick( rng.Uniform() * total );
		if( slot < 0 )
			break;
		const int cell = cellOf[ static_cast< size_t >( slot ) ];

		int parentCell = -1;
		WeightOf( cell, settings, parentCell );
		if( parentCell < 0 )
		{
			RemoveCandidate( cell );
			continue;
		}

		const int i = cell % nx, j = cell / nx;
		Node node;
		node.cell    = cell;
		node.x       = static_cast< float >( X( i ) );
		node.y       = static_cast< float >( Y( j ) );
		node.born    = now;
		node.lastHot = now;
		if( kind[ static_cast< size_t >( parentCell ) ] == Cell::Channel )
			node.parent = nodeAt[ static_cast< size_t >( parentCell ) ];
		else
		{
			node.parent = -1;
			node.ax     = static_cast< float >( X( parentCell % nx ) );
			node.ay     = static_cast< float >( Y( parentCell / nx ) );
		}
		const float px = node.parent >= 0 ? tree.Nodes()[ static_cast< size_t >( node.parent ) ].x : node.ax;
		const float py = node.parent >= 0 ? tree.Nodes()[ static_cast< size_t >( node.parent ) ].y : node.ay;

		int groundCell = -1;
		if( TouchesGround( cell, groundCell ) )
		{
			node.grounded = true;
			node.gx       = static_cast< float >( X( groundCell % nx ) );
			node.gy       = static_cast< float >( Y( groundCell / nx ) );
		}

		const int index = tree.Add( node );
		kind[ static_cast< size_t >( cell ) ]   = Cell::Channel;
		nodeAt[ static_cast< size_t >( cell ) ] = index;
		lattice.Hold( cell, 1.0f );
		RemoveCandidate( cell );

		++result.grown;
		result.length += std::hypot( node.x - px, node.y - py );

		if( node.grounded )
		{
			result.reachedGround = true;
			result.groundNode    = index;
			++result.connections;
			if( settings.stopAtGround || result.connections >= settings.maxConnections )
				break;
			// The circuit is complete: the path is a resistor now, not a tip, and
			// the rest of the channel grows round it.
			// No re-solve here: the arc is no longer anyone's parent, so the
			// candidates it leaves drop out at the rebuild, and the next
			// refresh puts its new potential into the field.
			Conduct( tree, index, lattice.Potential( groundCell ) );
			Rebuild( settings );
			continue;
		}

		// The field near the new tip, then the candidates it made and the
		// weights it moved.
		lattice.Relax( i, j, reach, settings.relaxSweeps );
		for( int k = 0; k < ( settings.diagonal ? 8 : 4 ); ++k )
		{
			const int n = lattice.Index( i + kDx[ k ], j + kDy[ k ] );
			if( kind[ static_cast< size_t >( n ) ] == Cell::Free
			    && ( settings.allowed == nullptr || ( *settings.allowed )[ static_cast< size_t >( n ) ] ) )
				AddCandidate( n );
		}

		if( ++sinceSolve >= settings.refreshEvery )
		{
			lattice.Solve( settings.tolerance );
			sinceSolve = 0;
			Rebuild( settings );
			continue;
		}

		const int x0w = std::max( 1, i - reach - 1 ), x1w = std::min( nx - 2, i + reach + 1 );
		const int y0w = std::max( 1, j - reach - 1 ), y1w = std::min( lattice.Ny() - 2, j + reach + 1 );
		for( int y = y0w; y <= y1w; ++y )
			for( int x = x0w; x <= x1w; ++x )
			{
				const int c = lattice.Index( x, y );
				const int s = slotOf[ static_cast< size_t >( c ) ];
				if( s < 0 )
					continue;
				int parent = -1;
				SetWeight( s, WeightOf( c, settings, parent ) );
			}
	}
	return result;
}
} // namespace flyback

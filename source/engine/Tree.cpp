#include "Tree.h"

#include <algorithm>
#include <cmath>

namespace flyback
{
int Tree::Add( const Node& node )
{
	nodes.push_back( node );
	Node& added = nodes.back();
	added.root  = added.parent < 0 ? static_cast< int >( nodes.size() ) - 1 : nodes[ static_cast< size_t >( added.parent ) ].root;
	return static_cast< int >( nodes.size() ) - 1;
}

void Tree::Currents( double total, double groundWeight )
{
	const size_t n = nodes.size();
	for( Node& node : nodes )
	{
		node.children = 0;
		node.weight   = 0.0;
	}
	for( size_t i = 0; i < n; ++i )
		if( nodes[ i ].alive && nodes[ i ].parent >= 0 )
			++nodes[ static_cast< size_t >( nodes[ i ].parent ) ].children;

	// Children always come after their parents -- a site can only grow from
	// one that is already there -- so one backwards pass accumulates every
	// subtree.
	for( size_t k = n; k-- > 0; )
	{
		Node& node = nodes[ k ];
		if( !node.alive )
			continue;
		if( node.children == 0 )
			node.weight += node.grounded ? groundWeight : kTipWeight;
		else if( node.grounded )
			node.weight += groundWeight;
		if( node.parent >= 0 )
			nodes[ static_cast< size_t >( node.parent ) ].weight += node.weight;
	}

	double rootWeight = 0.0;
	for( const Node& node : nodes )
		if( node.alive && node.parent < 0 )
			rootWeight += node.weight;

	const double unit = rootWeight > 0.0 ? total / rootWeight : 0.0;
	lastUnit          = unit;
	lastGroundWeight  = groundWeight;
	for( Node& node : nodes )
		node.current = node.alive ? static_cast< float >( unit * node.weight ) : 0.0f;
}

double Tree::WorstKirchhoff() const
{
	// What each node sheds on its own, from what it IS -- a free tip, a
	// grounded tip, or neither -- and not from the weights Currents() summed.
	// So this checks the accumulation rather than restating it.
	std::vector< double > outflow( nodes.size(), 0.0 );
	for( size_t i = 0; i < nodes.size(); ++i )
		if( nodes[ i ].alive && nodes[ i ].parent >= 0 )
			outflow[ static_cast< size_t >( nodes[ i ].parent ) ] += nodes[ i ].current;

	double worst = 0.0;
	for( size_t i = 0; i < nodes.size(); ++i )
	{
		const Node& node = nodes[ i ];
		if( !node.alive || node.current <= 0.0f )
			continue;
		double own = 0.0;
		if( node.grounded )
			own = lastGroundWeight * lastUnit;
		else if( node.children == 0 )
			own = kTipWeight * lastUnit;
		const double in  = node.current;
		const double out = own + outflow[ i ];
		worst            = std::max( worst, std::fabs( in - out ) / in );
	}
	return worst;
}

double Tree::TipSpread() const
{
	float lo = 1e30f, hi = 0.0f;
	for( const Node& node : nodes )
		if( node.alive && node.children == 0 && !node.grounded )
		{
			lo = std::min( lo, node.current );
			hi = std::max( hi, node.current );
		}
	return hi > 0.0f ? static_cast< double >( hi - lo ) / hi : 0.0;
}

double Tree::LongestPath() const
{
	std::vector< double > along( nodes.size(), 0.0 );
	double longest = 0.0;
	for( size_t i = 0; i < nodes.size(); ++i )
	{
		const Node& node = nodes[ i ];
		if( !node.alive )
			continue;
		const float px = node.parent >= 0 ? nodes[ static_cast< size_t >( node.parent ) ].x : node.ax;
		const float py = node.parent >= 0 ? nodes[ static_cast< size_t >( node.parent ) ].y : node.ay;
		const double base = node.parent >= 0 ? along[ static_cast< size_t >( node.parent ) ] : 0.0;
		along[ i ]        = base + std::hypot( node.x - px, node.y - py );
		longest           = std::max( longest, along[ i ] );
	}
	return longest;
}

double Tree::TotalLength() const
{
	double total = 0.0;
	for( const Node& node : nodes )
	{
		if( !node.alive )
			continue;
		const float px = node.parent >= 0 ? nodes[ static_cast< size_t >( node.parent ) ].x : node.ax;
		const float py = node.parent >= 0 ? nodes[ static_cast< size_t >( node.parent ) ].y : node.ay;
		total += std::hypot( node.x - px, node.y - py );
	}
	return total;
}

void Tree::Compact()
{
	std::vector< int > remap( nodes.size(), -1 );
	std::vector< Node > kept;
	kept.reserve( nodes.size() );
	for( size_t i = 0; i < nodes.size(); ++i )
	{
		Node node = nodes[ i ];
		if( !node.alive )
			continue;
		if( node.parent >= 0 )
		{
			const int mapped = remap[ static_cast< size_t >( node.parent ) ];
			if( mapped < 0 )
				continue;// its parent went, so it goes
			node.parent = mapped;
		}
		remap[ i ] = static_cast< int >( kept.size() );
		kept.push_back( node );
	}
	for( Node& node : kept )
		node.root = node.parent < 0 ? -1 : node.root;
	for( size_t i = 0; i < kept.size(); ++i )
		kept[ i ].root = kept[ i ].parent < 0 ? static_cast< int >( i ) : kept[ static_cast< size_t >( kept[ i ].parent ) ].root;
	nodes.swap( kept );
}

bool Tree::Connected() const
{
	for( const Node& node : nodes )
		if( node.alive && node.grounded )
			return true;
	return false;
}
} // namespace flyback

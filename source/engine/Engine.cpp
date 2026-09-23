#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace flyback
{
using namespace physics;

namespace
{
constexpr double kHuge = 1e30;

/// The luminous radius of a channel carrying `amps`: 1 mm at 0.1 A, going as
/// sqrt(I) (Tree.h says why). The renderer never draws narrower than a
/// pixel, so below that only the channel's light matters, not its width.
float LuminousRadius( double amps )
{
	return static_cast< float >( 1.0e-3 * std::sqrt( std::max( amps, 0.0 ) / 0.1 ) );
}

double Hypot( double x, double y )
{
	return std::sqrt( x * x + y * y );
}

double SegmentDistance( double px, double py, double ax, double ay, double bx, double by )
{
	const double dx = bx - ax, dy = by - ay;
	const double l2 = dx * dx + dy * dy;
	double t        = l2 > 0.0 ? ( ( px - ax ) * dx + ( py - ay ) * dy ) / l2 : 0.0;
	t               = std::clamp( t, 0.0, 1.0 );
	return Hypot( px - ( ax + t * dx ), py - ( ay + t * dy ) );
}
} // namespace

const char* MachineName( Machine machine )
{
	switch( machine )
	{
	case Machine::Ladder: return "Jacob's Ladder";
	case Machine::Tesla: return "Tesla Coil";
	case Machine::VanDeGraaff: return "Van de Graaff";
	case Machine::Globe: return "Plasma Globe";
	case Machine::Lichtenberg: return "Lichtenberg";
	default: return "?";
	}
}

double SceneHeight( Machine machine )
{
	switch( machine )
	{
	case Machine::Ladder: return 0.52;
	case Machine::Tesla: return 2.00;
	case Machine::VanDeGraaff: return 0.70;
	case Machine::Globe: return 0.50;
	case Machine::Lichtenberg: return 0.36;
	default: return 1.0;
	}
}

//===========================================================================
// What every machine shares: a field, a tree, a stream of random numbers,
// painting, and turning a tree into light.
//===========================================================================
class MachineBase
{
public:
	virtual ~MachineBase() = default;

	/// Take new settings. Repaints the field if anything it paints changed.
	void Setup( const Settings& next )
	{
		const bool seedChanged = next.seed != s.seed || !configured;
		const bool latticeChanged =
			!configured || next.cellsHigh != s.cellsHigh || std::fabs( next.aspect - s.aspect ) > 1e-6;
		s = next;
		if( seedChanged )
			rng.Seed( 0x9e3779b97f4a7c15ULL * ( s.seed + 1u ) + static_cast< uint64_t >( s.machine ) );
		Configure();
		const uint64_t key = PaintKey();
		if( latticeChanged || key != paintKey )
		{
			Repaint( latticeChanged );
			paintKey = key;
		}
		configured = true;
	}

	virtual void Configure()
	{
	}
	virtual uint64_t PaintKey() const = 0;
	virtual void PaintApparatus() = 0;
	virtual float RingPotential() const
	{
		return 0.0f;
	}
	/// How wide a field the machine needs. The whole frame by default; a
	/// machine whose discharge cannot leave its apparatus solves a square.
	virtual double FieldWidth() const
	{
		return W();
	}

	virtual void Step( double t0, double t1, double exposeFrom, int frame, Frame& out, std::vector< Event >& log ) = 0;
	virtual void Fire( double t )      = 0;
	virtual void Restart( double t )   = 0;
	/// The host's clock jumped: move any clock the machine keeps to `t`
	/// without replaying the gap.
	virtual void Rebase( double t )
	{
		( void )t;
	}
	virtual void Shapes( Frame& out ) const = 0;

	Field field;
	Tree tree;
	Tree lastEmitted;
	Pcg32 rng;
	Settings s;

protected:
	double H() const
	{
		return SceneHeight( s.machine );
	}
	double W() const
	{
		return H() * s.aspect;
	}

	//-----------------------------------------------------------------------
	// Frame <-> scene. Frame coordinates are in frame heights, centred, y up;
	// the layout places the scene in the frame.
	//-----------------------------------------------------------------------
	void FrameToScene( double fx, double fy, double& x, double& y ) const
	{
		const double u = ( fx - 0.5 ) * s.aspect - s.posX;
		const double v = ( fy - 0.5 ) - s.posY;
		const double c = std::cos( -s.rotation ), sn = std::sin( -s.rotation );
		const double k = H() / std::max( s.scale, 1e-3 );
		x              = ( c * u - sn * v ) * k;
		y              = ( sn * u + c * v ) * k;
	}

	void SceneToFrame( double x, double y, double& fx, double& fy ) const
	{
		const double k = std::max( s.scale, 1e-3 ) / H();
		const double c = std::cos( s.rotation ), sn = std::sin( s.rotation );
		const double u = ( c * x - sn * y ) * k + s.posX;
		const double v = ( sn * x + c * y ) * k + s.posY;
		fx             = u / s.aspect + 0.5;
		fy             = v + 0.5;
	}

	//-----------------------------------------------------------------------
	// Painting.
	//-----------------------------------------------------------------------
	enum class Paint
	{
		Source,
		Ground,
		Held
	};

	void PaintCell( int cell, Paint paint, float potential )
	{
		switch( paint )
		{
		case Paint::Source: field.PaintSource( cell ); break;
		case Paint::Ground: field.PaintGround( cell, potential ); break;
		case Paint::Held: field.PaintHeld( cell, potential ); break;
		}
	}

	/// Every site within `r` of the segment -- and never less than 0.72 of a
	/// step, so a thin wire is still a connected line of held sites.
	void PaintCapsule( double x0, double y0, double x1, double y1, double r, Paint paint, float potential )
	{
		const double h     = field.Step();
		const double reach = std::max( r, 0.72 * h );
		ForBox( std::min( x0, x1 ) - reach, std::min( y0, y1 ) - reach, std::max( x0, x1 ) + reach,
		        std::max( y0, y1 ) + reach, [ & ]( int cell, double x, double y ) {
			        if( SegmentDistance( x, y, x0, y0, x1, y1 ) <= reach )
				        PaintCell( cell, paint, potential );
		        } );
	}

	void PaintDisc( double cx, double cy, double r, Paint paint, float potential )
	{
		PaintCapsule( cx, cy, cx, cy, r, paint, potential );
	}

	template< typename F >
	void ForBox( double xa, double ya, double xb, double yb, F&& f )
	{
		const double h = field.Step();
		const int i0   = std::max( 1, static_cast< int >( std::floor( ( xa - field.X( 0 ) ) / h ) ) );
		const int i1   = std::min( field.Nx() - 2, static_cast< int >( std::ceil( ( xb - field.X( 0 ) ) / h ) ) );
		const int j0   = std::max( 1, static_cast< int >( std::floor( ( ya - field.Y( 0 ) ) / h ) ) );
		const int j1   = std::min( field.Ny() - 2, static_cast< int >( std::ceil( ( yb - field.Y( 0 ) ) / h ) ) );
		for( int j = j0; j <= j1; ++j )
			for( int i = i0; i <= i1; ++i )
				f( field.Grid().Index( i, j ), field.X( i ), field.Y( j ) );
	}

	/// Over: every free site whose place in the frame is bright in the clip
	/// becomes ground.
	void PaintClip()
	{
		if( s.clip == nullptr || s.clipW <= 0 || s.clipH <= 0 )
			return;
		const std::vector< uint8_t >& mask = *s.clip;
		for( int j = 1; j < field.Ny() - 1; ++j )
			for( int i = 1; i < field.Nx() - 1; ++i )
			{
				const int cell = field.Grid().Index( i, j );
				if( field.Kind( cell ) != Cell::Free )
					continue;
				double fx, fy;
				SceneToFrame( field.X( i ), field.Y( j ), fx, fy );
				if( fx < 0.0 || fy < 0.0 || fx >= 1.0 || fy >= 1.0 )
					continue;
				const int mx = std::min( s.clipW - 1, static_cast< int >( fx * s.clipW ) );
				const int my = std::min( s.clipH - 1, static_cast< int >( fy * s.clipH ) );
				if( mask[ static_cast< size_t >( my * s.clipW + mx ) ] )
					field.PaintGround( cell, 0.0f );
			}
	}

	void Repaint( bool resetLattice )
	{
		if( resetLattice )
		{
			field.Reset( std::min( W(), FieldWidth() ), H(), s.cellsHigh, RingPotential() );
			tree.Clear();
		}
		else
			field.ClearConductors();
		PaintApparatus();
		PaintClip();
		field.Adopt( tree );
		tree.Compact();
		field.Adopt( tree );
	}

	uint64_t ClipKey() const
	{
		return s.clip != nullptr ? s.clipStamp * 1000003ULL + 17ULL : 0ULL;
	}

	static uint64_t Mix( uint64_t key, double value )
	{
		const uint64_t bits = static_cast< uint64_t >( std::llround( value * 1e6 ) );
		return ( key ^ bits ) * 0x100000001b3ULL + 0x9e3779b97f4a7c15ULL;
	}

	//-----------------------------------------------------------------------
	// Light.
	//
	// The tree's currents by Kirchhoff, then the light shared out as
	// I * length (Tree.h), each segment's joules summing to exactly `light`.
	// A root starts at its attachment point on the electrode; a grounded tip
	// gets one more segment, to where it touched.
	//
	// What the renderer draws is not the bare lattice: each node is moved by
	// up to 0.3 of a step, keyed on its cell so it stays put while the channel
	// does, and each segment is split in four by midpoint displacement, which
	// never moves its ends. Both are the look, not the physics; the light each
	// piece carries is its share of the length, so the sum is unchanged.
	//-----------------------------------------------------------------------
	void Emit( Tree& from, double amps, double groundWeight, double light, Frame& out )
	{
		from.Currents( amps, groundWeight );
		if( !from.Empty() )
			lastEmitted = from;
		const std::vector< Node >& nodes = from.Nodes();
		if( nodes.empty() || light <= 0.0 )
			return;

		const double h   = field.Step();
		const uint32_t k = s.seed * 2654435761u;

		auto jittered = [ & ]( const Node& n, double& x, double& y ) {
			const uint32_t c = static_cast< uint32_t >( n.cell );
			x                = n.x + 0.3 * h * Signed( Hash( c, k ) );
			y                = n.y + 0.3 * h * Signed( Hash( c, k + 1u ) );
		};

		struct Piece
		{
			double x0, y0, x1, y1;
			double amps;
			uint32_t key;
		};
		std::vector< Piece > pieces;
		pieces.reserve( nodes.size() + 8 );
		double norm = 0.0;
		for( const Node& n : nodes )
		{
			if( !n.alive || n.current <= 0.0f )
				continue;
			double x1, y1, x0, y0;
			jittered( n, x1, y1 );
			if( n.parent >= 0 )
				jittered( nodes[ static_cast< size_t >( n.parent ) ], x0, y0 );
			else
			{
				x0 = n.ax;
				y0 = n.ay;
			}
			pieces.push_back( { x0, y0, x1, y1, n.current, static_cast< uint32_t >( n.cell ) } );
			norm += n.current * Hypot( x1 - x0, y1 - y0 );
			if( n.grounded )
			{
				pieces.push_back( { x1, y1, n.gx, n.gy, n.current, static_cast< uint32_t >( n.cell ) ^ 0x55555555u } );
				norm += n.current * Hypot( n.gx - x1, n.gy - y1 );
			}
		}
		if( norm <= 0.0 )
			return;

		for( const Piece& p : pieces )
		{
			// Four pieces by two levels of midpoint displacement.
			double px[ 5 ], py[ 5 ];
			px[ 0 ]         = p.x0;
			py[ 0 ]         = p.y0;
			px[ 4 ]         = p.x1;
			py[ 4 ]         = p.y1;
			const double L  = Hypot( p.x1 - p.x0, p.y1 - p.y0 );
			const double nx = L > 0.0 ? -( p.y1 - p.y0 ) / L : 0.0, ny = L > 0.0 ? ( p.x1 - p.x0 ) / L : 0.0;
			const double a  = 0.16 * L * Signed( Hash( p.key, k + 7u ) );
			px[ 2 ]         = 0.5 * ( px[ 0 ] + px[ 4 ] ) + nx * a;
			py[ 2 ]         = 0.5 * ( py[ 0 ] + py[ 4 ] ) + ny * a;
			for( int q = 0; q < 2; ++q )
			{
				const int lo = q * 2, hi = lo + 2;
				const double b = 0.08 * L * Signed( Hash( p.key, k + 11u + static_cast< uint32_t >( q ) ) );
				px[ lo + 1 ]   = 0.5 * ( px[ lo ] + px[ hi ] ) + nx * b;
				py[ lo + 1 ]   = 0.5 * ( py[ lo ] + py[ hi ] ) + ny * b;
			}
			double sub = 0.0;
			for( int q = 0; q < 4; ++q )
				sub += Hypot( px[ q + 1 ] - px[ q ], py[ q + 1 ] - py[ q ] );
			const double share = light * p.amps * L / norm;
			const float radius = LuminousRadius( p.amps );
			const float thermal = static_cast< float >( p.amps / ( p.amps + kThermalAmps ) );
			for( int q = 0; q < 4; ++q )
			{
				const double piece = Hypot( px[ q + 1 ] - px[ q ], py[ q + 1 ] - py[ q ] );
				const double j     = sub > 0.0 ? share * piece / sub : share * 0.25;
				out.segments.push_back( { static_cast< float >( px[ q ] ), static_cast< float >( py[ q ] ),
				                          static_cast< float >( px[ q + 1 ] ), static_cast< float >( py[ q + 1 ] ),
				                          static_cast< float >( j ), radius, thermal } );
			}
		}
		out.joules += light;
	}

	/// The part of [a, b] that the shutter saw.
	static double Exposed( double a, double b, double exposeFrom )
	{
		return std::max( 0.0, b - std::max( a, exposeFrom ) );
	}

	GrowthSettings Growth( double eta, bool stopAtGround )
	{
		GrowthSettings g;
		g.eta          = eta;
		g.stopAtGround = stopAtGround;
		// A fresh draw of the air's inhomogeneity for every burst, from the
		// seeded stream: deterministic, and never the same twice.
		g.disorderKey = rng.Next();
		return g;
	}

	bool configured   = false;
	uint64_t paintKey = 0;
};

//===========================================================================
// Tesla coil.
//===========================================================================
class TeslaMachine : public MachineBase
{
public:
	// The apparatus, in scene metres. The toroid sits just above the middle
	// of the frame so its streamers have room both ways, and the floor is
	// 0.77 m below it: within reach of the nominal coil's longest streamer
	// (1.27 m), not within reach of most.
	static constexpr double kToroidY     = 0.05;
	static constexpr double kSecondaryR  = 0.065;
	static constexpr double kBaseY       = -0.62;
	static constexpr double kFloorY      = -0.72;
	static constexpr double kTargetR     = 0.04;
	static constexpr double kBaseHalf    = 0.20;
	static constexpr double kMinorRatio  = 0.32;
	static constexpr double kBangSeconds = 100e-6;///< the ring-down that carries a bang's charge
	static constexpr double kReach       = 2.50;  ///< nominal new channel per bang, all branches, metres
	static constexpr double kGroundWeight = 20.0; ///< a strike carries twenty tips' worth

	double Minor() const
	{
		return kMinorRatio * s.topload;
	}

	uint64_t PaintKey() const override
	{
		uint64_t k = 1;
		k          = Mix( k, s.topload );
		k          = Mix( k, s.target );
		if( s.target == 2 )
		{
			k = Mix( k, s.targetX );
			k = Mix( k, s.targetY );
		}
		k = Mix( k, s.posX );
		k = Mix( k, s.posY );
		k = Mix( k, s.scale );
		k = Mix( k, s.rotation );
		return k ^ ClipKey();
	}

	void TargetScene( double& x, double& y ) const
	{
		FrameToScene( s.targetX, s.targetY, x, y );
	}

	void PaintApparatus() override
	{
		// The toroid, side on: a capsule. The discharge grows from here.
		PaintCapsule( -s.topload, kToroidY, s.topload, kToroidY, Minor(), Paint::Source, 1.0f );
		// The secondary: a conductor whose potential rises from 0 at the base to
		// the topload's at the top -- as a quarter wave, sin(pi/2 . y), which is
		// how a resonant secondary's voltage is distributed. Its field shapes
		// the growth, but it is held INERT, not ground: a streamer cannot
		// strike it. With it as ground every downward streamer raced down the
		// coil (469 of 469 in `--over`'s control), which on a real coil is the
		// sign of one badly overdriven. So racing sparks are not modelled; the
		// strike rail at the base is.
		const double top = kToroidY - Minor();
		ForBox( -kSecondaryR, kBaseY, kSecondaryR, top, [ & ]( int cell, double, double y ) {
			const double u = std::clamp( ( y - kBaseY ) / ( top - kBaseY ), 0.0, 1.0 );
			PaintCell( cell, Paint::Held, static_cast< float >( 0.97 * std::sin( 0.5 * kPi * u ) ) );
		} );
		// The base and the strike rail round it: grounded.
		ForBox( -kBaseHalf, kBaseY - 0.06, kBaseHalf, kBaseY, [ & ]( int cell, double, double ) { PaintCell( cell, Paint::Ground, 0.0f ); } );
		if( s.target == 1 )
			ForBox( -kHuge, -kHuge, kHuge, kFloorY, [ & ]( int cell, double, double ) { PaintCell( cell, Paint::Ground, 0.0f ); } );
		else if( s.target == 2 )
		{
			double tx, ty;
			TargetScene( tx, ty );
			PaintDisc( tx, ty, kTargetR, Paint::Ground, 0.0f );
		}
	}

	void Configure() override
	{
		const double period = s.bps > 0.0 ? 1.0 / s.bps : 0.0;
		if( period != bangPeriod )
		{
			// Re-anchor on the last bang so a BPS change never leaps.
			anchor     = lastBang > -kHuge ? lastBang : anchor;
			bangIndex  = 0;
			bangPeriod = period;
		}
	}

	double TopVolts() const
	{
		return ToploadVolts( s.supply, s.topload );
	}
	double BangEnergy() const
	{
		return BangJoules( s.supply, s.topload );
	}
	double BangAmps() const
	{
		return ToploadCapacitance( s.topload ) * TopVolts() / kBangSeconds;
	}
	/// The longest a streamer can be: the topload's voltage over the field a
	/// streamer needs to propagate.
	double MaxLength() const
	{
		return TopVolts() / kStreamerField;
	}

	void Restart( double t ) override
	{
		tree.Clear();
		field.Adopt( tree );
		anchor    = t;
		bangIndex = 0;
		lastBang  = -kHuge;
		pending.clear();
	}

	void Fire( double t ) override
	{
		pending.push_back( t );
	}

	void Rebase( double t ) override
	{
		anchor    = t;
		bangIndex = 0;
		pending.clear();
	}

	void Step( double t0, double t1, double exposeFrom, int frame, Frame& out, std::vector< Event >& log ) override
	{
		// Never replay a gap: a clock bang due before this frame began is one
		// that a jump skipped.
		if( bangPeriod > 0.0 && anchor + static_cast< double >( bangIndex + 1 ) / s.bps < t0 - 1e-9 )
			Rebase( t0 );
		for( ;; )
		{
			double next = kHuge;
			bool clock  = false;
			if( bangPeriod > 0.0 )
			{
				// k / BPS, not k * (1 / BPS): at 37.5 BPS the product puts the
				// 2250th bang at 60.000000000000007 s and it misses the minute.
				next  = anchor + static_cast< double >( bangIndex + 1 ) / s.bps;
				clock = true;
			}
			double fired = kHuge;
			if( !pending.empty() )
				fired = *std::min_element( pending.begin(), pending.end() );
			const double t = std::min( next, fired );
			if( t > t1 )
				break;
			if( fired <= next )
			{
				pending.erase( std::min_element( pending.begin(), pending.end() ) );
				clock = false;
			}
			if( clock )
				++bangIndex;
			Bang( t, t > exposeFrom, frame, out, log );
		}
	}

	void Bang( double t, bool exposed, int frame, Frame& out, std::vector< Event >& log )
	{
		// Channel memory: a segment is still hot enough to carry the next bang
		// if the gap is shorter than its own cooling time. A channel's radius
		// goes as sqrt(I), thermal diffusion time as radius^2, so a segment
		// carrying a fraction f of its root's current cools in f tau. The
		// trunk is the last to go and the tips the first.
		const double dt = t - lastBang;
		if( !tree.Empty() )
		{
			std::vector< Node >& nodes = tree.Nodes();
			std::vector< float > rootAmps( nodes.size(), 0.0f );
			for( size_t i = 0; i < nodes.size(); ++i )
				if( nodes[ i ].parent < 0 )
					rootAmps[ i ] = nodes[ i ].current;
			for( Node& n : nodes )
			{
				const float root = n.root >= 0 ? rootAmps[ static_cast< size_t >( n.root ) ] : 0.0f;
				const double f   = root > 0.0f ? n.current / root : 0.0;
				if( !( dt < s.memory * f ) )
				{
					n.alive = false;
					field.Forget( n );
				}
			}
			for( Node& n : nodes )
				if( n.alive && n.parent >= 0 && !nodes[ static_cast< size_t >( n.parent ) ].alive )
				{
					n.alive = false;
					field.Forget( n );
				}
			tree.Compact();
			field.Adopt( tree );
		}

		// New channel: this bang's reach, but no streamer longer than the
		// topload's voltage can hold up.
		const double longest = tree.LongestPath();
		const double budget  = std::min( kReach * s.reach, std::max( 0.0, MaxLength() - longest ) );
		GrowthResult grown;
		if( budget > 0.25 * field.Step() && !tree.Connected() )
			grown = field.Grow( tree, 100000, budget, rng, t, Growth( s.eta, true ) );

		for( Node& n : tree.Nodes() )
			n.lastHot = t;

		Event e;
		e.time      = t;
		e.joules    = BangEnergy();
		e.connected = tree.Connected();
		e.exposed   = exposed;
		e.frame     = exposed ? frame : -1;

		// Currents are needed for the next bang's cooling whether or not
		// anyone sees this one.
		Frame scratch;
		Emit( tree, BangAmps(), kGroundWeight, e.joules * s.efficiency, exposed ? out : scratch );
		if( exposed )
		{
			e.light = e.joules * s.efficiency;
			++out.events;
		}
		e.length = tree.LongestPath();
		e.nodes  = static_cast< int >( tree.Size() );
		for( const Node& n : tree.Nodes() )
		{
			if( n.children == 0 && !n.grounded )
				++e.branches;
			if( n.grounded )
			{
				e.groundX = n.gx;
				e.groundY = n.gy;
			}
		}
		log.push_back( e );
		lastBang = t;

		// A strike is over as soon as it has happened: the channel that
		// reached ground is what the next bang re-uses if it is still hot, and
		// it is re-tested for cooling like any other.
		if( tree.Connected() )
			for( Node& n : tree.Nodes() )
				n.grounded = false;
		( void )grown;
	}

	void Shapes( Frame& out ) const override
	{
		const float r = static_cast< float >( s.topload ), m = static_cast< float >( Minor() );
		const float top = static_cast< float >( kToroidY - Minor() );
		if( s.target == 1 )
			out.shapes.push_back( { Shape::Box, -50.0f, -50.0f, 50.0f, static_cast< float >( kFloorY ), 0.0f, Shape::Floor } );
		out.shapes.push_back( { Shape::Box, static_cast< float >( -kBaseHalf ), static_cast< float >( kBaseY - 0.06 ), static_cast< float >( kBaseHalf ),
		                        static_cast< float >( kBaseY ), 0.0f, Shape::Dull } );
		out.shapes.push_back( { Shape::Box, static_cast< float >( -kSecondaryR ), static_cast< float >( kBaseY ),
		                        static_cast< float >( kSecondaryR ), top, 0.0f, Shape::Copper } );
		out.shapes.push_back( { Shape::Capsule, -r, static_cast< float >( kToroidY ), r, static_cast< float >( kToroidY ), m, Shape::Metal } );
		if( s.target == 2 )
		{
			double tx, ty;
			TargetScene( tx, ty );
			out.shapes.push_back( { Shape::Capsule, static_cast< float >( tx ), static_cast< float >( ty ), static_cast< float >( tx ),
			                        static_cast< float >( kFloorY ), 0.008f, Shape::Metal } );
			out.shapes.push_back( { Shape::Disc, static_cast< float >( tx ), static_cast< float >( ty ), 0, 0,
			                        static_cast< float >( kTargetR ), Shape::Metal } );
		}
	}

	double anchor     = 0.0;
	long bangIndex    = 0;
	double bangPeriod = -1.0;
	double lastBang   = -kHuge;
	std::vector< double > pending;
};

//===========================================================================
// Jacob's ladder.
//===========================================================================
class LadderMachine : public MachineBase
{
public:
	static constexpr double kBottomY   = -0.19;
	static constexpr double kBottomGap = 0.003;
	static constexpr double kRodRadius = 0.003;
	static constexpr double kSubstep   = 0.001;
	static constexpr double kGroundWeight = 50.0;

	LadderProbe probe;

	double Half() const
	{
		return 0.5 * s.rodSpread;
	}
	void RodA( double& x0, double& y0, double& x1, double& y1 ) const
	{
		x0 = -0.5 * kBottomGap;
		y0 = kBottomY;
		x1 = x0 - s.rodLength * std::sin( Half() );
		y1 = y0 + s.rodLength * std::cos( Half() );
	}
	void RodB( double& x0, double& y0, double& x1, double& y1 ) const
	{
		x0 = 0.5 * kBottomGap;
		y0 = kBottomY;
		x1 = x0 + s.rodLength * std::sin( Half() );
		y1 = y0 + s.rodLength * std::cos( Half() );
	}
	/// The point on a rod at height `y` (clamped to the rod).
	void OnRod( bool a, double y, double& x, double& yy ) const
	{
		double x0, y0, x1, y1;
		if( a )
			RodA( x0, y0, x1, y1 );
		else
			RodB( x0, y0, x1, y1 );
		const double t = std::clamp( ( y - y0 ) / ( y1 - y0 ), 0.0, 1.0 );
		x              = x0 + t * ( x1 - x0 );
		yy             = y0 + t * ( y1 - y0 );
	}
	/// The nearest point on a rod to (px, py), and how far along it is (0..1).
	double Project( bool a, double px, double py, double& x, double& y ) const
	{
		double x0, y0, x1, y1;
		if( a )
			RodA( x0, y0, x1, y1 );
		else
			RodB( x0, y0, x1, y1 );
		const double dx = x1 - x0, dy = y1 - y0;
		const double t  = ( ( px - x0 ) * dx + ( py - y0 ) * dy ) / ( dx * dx + dy * dy );
		const double c  = std::clamp( t, 0.0, 1.0 );
		x               = x0 + c * dx;
		y               = y0 + c * dy;
		return t;
	}

	float RingPotential() const override
	{
		return 0.5f;
	}
	double FieldWidth() const override
	{
		return H();
	}

	uint64_t PaintKey() const override
	{
		// Painted every frame anyway: the source window follows the foot.
		return ++paintCounter;
	}

	void PaintApparatus() override
	{
		double ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
		RodA( ax0, ay0, ax1, ay1 );
		RodB( bx0, by0, bx1, by1 );
		// Rod A is the live rod. Only the part of it where the arc's foot is
		// can start a channel; the rest is a conductor at the same potential.
		PaintCapsule( ax0, ay0, ax1, ay1, kRodRadius, Paint::Held, 1.0f );
		if( !column.empty() )
		{
			const double fx = column.front().x, fy = column.front().y, h = field.Step();
			ForBox( fx - 2 * h, fy - 2 * h, fx + 2 * h, fy + 2 * h, [ & ]( int cell, double x, double y ) {
				if( field.Kind( cell ) == Cell::Held && Hypot( x - fx, y - fy ) <= 1.6 * h )
					field.PaintSource( cell );
			} );
		}
		PaintCapsule( bx0, by0, bx1, by1, kRodRadius, Paint::Ground, 0.0f );
	}

	void Configure() override
	{
		probe.extinction = ExtinctionLength( ayrton, s.supply );
		probe.strikeGap  = StrikeGap( s.supply );
	}

	void Restart( double t ) override
	{
		column.clear();
		tree.Clear();
		probe.lit = false;
		lastT     = t;
	}

	void Fire( double t ) override
	{
		fireAt = t;
	}

	struct P
	{
		double x, y;
	};

	double Length() const
	{
		double l = 0.0;
		for( size_t i = 1; i < column.size(); ++i )
			l += Hypot( column[ i ].x - column[ i - 1 ].x, column[ i ].y - column[ i - 1 ].y );
		return l;
	}

	bool Strike( double t )
	{
		// The shortest gap is the bottom, and the supply can strike it only if
		// it is within V_oc / E_b.
		if( kBottomGap > probe.strikeGap )
			return false;
		double ax, ay, bx, by;
		OnRod( true, kBottomY, ax, ay );
		OnRod( false, kBottomY, bx, by );
		column.clear();
		const int n = 6;
		for( int i = 0; i <= n; ++i )
			column.push_back( { ax + ( bx - ax ) * i / n, ay + ( by - ay ) * i / n } );
		probe.lit = true;
		probe.strikes.push_back( t );
		probe.strikeHeights.push_back( 0.0 );
		Event e;
		e.time   = t;
		e.joules = 0.0;
		strikesThisFrame.push_back( e );
		return true;
	}

	void Extinguish( double t, double length, bool top = false )
	{
		probe.lit = false;
		probe.extinctions.push_back( t );
		probe.lengthsAt.push_back( length );
		probe.overTheTop.push_back( top ? 1 : 0 );
		column.clear();
	}

	/// The hot column rises at the buoyant speed. Its roots on the rods slide
	/// up more slowly -- the metal cools the column where it touches and the
	/// root has to keep re-forming -- at `rootSpeed` of it, and the column
	/// blends from the roots' speed to the free rise across a boundary layer
	/// `kLayer` thick. The middle outruns the feet, and the arc bows upward.
	/// Wind blows the free column sideways.
	static constexpr double kLayer = 0.005;

	void Advect( double dt )
	{
		const size_t n = column.size();
		if( n < 2 )
			return;
		const double k = std::clamp( probe.rootSpeed, 0.0, 1.0 );
		for( size_t i = 1; i + 1 < n; ++i )
		{
			P& p = column[ i ];
			double qx, qy;
			Project( true, p.x, p.y, qx, qy );
			const double da = Hypot( p.x - qx, p.y - qy );
			Project( false, p.x, p.y, qx, qy );
			const double db = Hypot( p.x - qx, p.y - qy );
			const double f  = 1.0 - ( 1.0 - k ) * std::exp( -std::min( da, db ) / kLayer );
			p.x += s.wind * f * dt;
			p.y += s.rise * f * dt;
		}
		// The roots climb the rods at k times the rise, in height.
		double x, y;
		OnRod( true, column.front().y + k * s.rise * dt, x, y );
		column.front() = { x, y };
		OnRod( false, column.back().y + k * s.rise * dt, x, y );
		column.back() = { x, y };

		// Keep the polyline fine enough to measure its length to a small
		// fraction of a lattice step.
		const double most = 0.25 * field.Step();
		std::vector< P > fine;
		fine.reserve( n + 8 );
		for( size_t i = 0; i < column.size(); ++i )
		{
			if( i > 0 )
			{
				const P a = column[ i - 1 ], b = column[ i ];
				const double l = Hypot( b.x - a.x, b.y - a.y );
				const int kk   = static_cast< int >( l / most );
				for( int q = 1; q <= kk; ++q )
				{
					const double t = static_cast< double >( q ) / ( kk + 1 );
					fine.push_back( { a.x + t * ( b.x - a.x ), a.y + t * ( b.y - a.y ) } );
				}
			}
			fine.push_back( column[ i ] );
		}
		column.swap( fine );
	}

	bool OffTheTop() const
	{
		double x0, y0, x1, y1;
		RodA( x0, y0, x1, y1 );
		return column.front().y >= y1 - 1e-9;
	}

	/// Regrow the channel inside the hot column: a breakdown-model walk at
	/// this machine's eta from the foot on rod A to rod B, allowed only in
	/// the cells within 1.8 steps of the column. It follows the column and
	/// wanders inside it, frame to frame.
	void Regrow( double t )
	{
		tree.Clear();
		if( column.size() < 2 )
			return;
		Repaint( false );
		const double h = field.Step();
		allowed.assign( static_cast< size_t >( field.Nx() * field.Ny() ), 0 );
		for( size_t i = 1; i < column.size(); ++i )
		{
			const P a = column[ i - 1 ], b = column[ i ];
			ForBox( std::min( a.x, b.x ) - 2 * h, std::min( a.y, b.y ) - 2 * h, std::max( a.x, b.x ) + 2 * h,
			        std::max( a.y, b.y ) + 2 * h, [ & ]( int cell, double x, double y ) {
				        if( SegmentDistance( x, y, a.x, a.y, b.x, b.y ) <= 1.8 * h )
					        allowed[ static_cast< size_t >( cell ) ] = 1;
			        } );
		}
		GrowthSettings g = Growth( s.eta, true );
		g.allowed        = &allowed;
		const int cap    = static_cast< int >( 6.0 * Length() / h ) + 16;
		const GrowthResult r = field.Grow( tree, cap, kHuge, rng, t, g );
		if( !r.reachedGround )
			tree.Clear();// the column itself is drawn instead
	}

	/// The arc as light: the regrown channel if it made it across, the
	/// column itself if not.
	void EmitArc( double light, double amps, Frame& out )
	{
		if( light <= 0.0 )
			return;
		if( !tree.Empty() && tree.Connected() )
		{
			Emit( tree, amps, kGroundWeight, light, out );
			return;
		}
		double total = Length();
		if( total <= 0.0 )
			return;
		for( size_t i = 1; i < column.size(); ++i )
		{
			const P a = column[ i - 1 ], b = column[ i ];
			const double l = Hypot( b.x - a.x, b.y - a.y );
			out.segments.push_back( { static_cast< float >( a.x ), static_cast< float >( a.y ), static_cast< float >( b.x ),
			                          static_cast< float >( b.y ), static_cast< float >( light * l / total ),
			                          LuminousRadius( amps ), static_cast< float >( amps / ( amps + kThermalAmps ) ) } );
		}
		out.joules += light;
	}

	void Step( double t0, double t1, double exposeFrom, int frame, Frame& out, std::vector< Event >& log ) override
	{
		strikesThisFrame.clear();
		double light = 0.0;
		double amps  = 0.0;
		if( !probe.lit && column.empty() )
			Strike( t0 );

		// Substeps of a millisecond, ending exactly on t1.
		const int steps = std::max( 1, static_cast< int >( std::ceil( ( t1 - t0 ) / kSubstep - 1e-9 ) ) );
		const double dt = ( t1 - t0 ) / steps;
		for( int k = 0; k < steps; ++k )
		{
			const double a = t0 + k * dt, b = a + dt;
			if( fireAt > -kHuge && fireAt <= b )
			{
				if( probe.lit )
					Extinguish( a, Length() );
				fireAt = -kHuge;
				Strike( a );
			}
			if( !probe.lit )
			{
				if( !Strike( a ) )
					continue;
			}
			const double before = Length();
			Advect( dt );
			const double after = Length();
			probe.length       = after;
			const double lStar = probe.extinction;

			const bool top = after < lStar && OffTheTop();
			if( after >= lStar || top )
			{
				// Where in the substep the column reached L*, linearly. (Running
				// off the top is simply the end of the substep.)
				const double f = top ? 1.0 : after > before ? std::clamp( ( lStar - before ) / ( after - before ), 0.0, 1.0 ) : 1.0;
				const double te = a + f * dt;
				const double I  = ArcCurrent( ayrton, s.supply, std::min( before, lStar ) );
				const double P  = ( s.supply.openVolts - I * s.supply.sourceOhms ) * I;
				light += P * Exposed( a, te, exposeFrom ) * s.efficiency;
				amps = std::max( amps, I );
				// The dying arc is drawn where it died, with the light it gave
				// in this frame, before the new one starts at the bottom.
				EmitArc( light, I, out );
				light = 0.0;
				Extinguish( top ? b : te, after, top );
				Strike( top ? b : te );
				continue;
			}

			const double I = ArcCurrent( ayrton, s.supply, after );
			probe.current  = I;
			probe.power    = ( s.supply.openVolts - I * s.supply.sourceOhms ) * I;
			light += probe.power * Exposed( a, b, exposeFrom ) * s.efficiency;
			amps = std::max( amps, I );
		}

		if( probe.lit && !column.empty() )
		{
			double apex = -kHuge;
			for( const P& p : column )
				apex = std::max( apex, p.y );
			probe.apex       = apex - kBottomY;
			probe.footHeight = column.front().y - kBottomY;
			Regrow( t1 );
			EmitArc( light, amps, out );
		}
		else
			tree.Clear();

		for( Event e : strikesThisFrame )
		{
			e.exposed = e.time > exposeFrom;
			e.frame   = frame;
			log.push_back( e );
		}
		if( light > 0.0 )
			++out.events;
	}

	void Shapes( Frame& out ) const override
	{
		double ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
		RodA( ax0, ay0, ax1, ay1 );
		RodB( bx0, by0, bx1, by1 );
		const float r = static_cast< float >( kRodRadius );
		out.shapes.push_back( { Shape::Capsule, static_cast< float >( ax0 ), static_cast< float >( ay0 ), static_cast< float >( ax1 ),
		                        static_cast< float >( ay1 ), r, Shape::Metal } );
		out.shapes.push_back( { Shape::Capsule, static_cast< float >( bx0 ), static_cast< float >( by0 ), static_cast< float >( bx1 ),
		                        static_cast< float >( by1 ), r, Shape::Metal } );
		// The rods' legs down into the insulating base.
		out.shapes.push_back( { Shape::Capsule, static_cast< float >( ax0 - 0.012 ), static_cast< float >( ay0 ), static_cast< float >( ax0 - 0.012 ),
		                        static_cast< float >( kBottomY - 0.06 ), r, Shape::Metal } );
		out.shapes.push_back( { Shape::Capsule, static_cast< float >( ax0 ), static_cast< float >( ay0 ), static_cast< float >( ax0 - 0.012 ),
		                        static_cast< float >( ay0 ), r, Shape::Metal } );
		out.shapes.push_back( { Shape::Capsule, static_cast< float >( bx0 + 0.012 ), static_cast< float >( by0 ), static_cast< float >( bx0 + 0.012 ),
		                        static_cast< float >( kBottomY - 0.06 ), r, Shape::Metal } );
		out.shapes.push_back( { Shape::Capsule, static_cast< float >( bx0 ), static_cast< float >( by0 ), static_cast< float >( bx0 + 0.012 ),
		                        static_cast< float >( by0 ), r, Shape::Metal } );
		out.shapes.push_back( { Shape::Box, -0.09f, static_cast< float >( kBottomY - 0.10 ), 0.09f, static_cast< float >( kBottomY - 0.06 ), 0.0f, Shape::Dull } );
	}

	Ayrton ayrton;
	std::vector< P > column;
	std::vector< uint8_t > allowed;
	std::vector< Event > strikesThisFrame;
	double fireAt  = -kHuge;
	double lastT   = 0.0;
	mutable uint64_t paintCounter = 0;
};

//===========================================================================
// Van de Graaff.
//===========================================================================
class VdgMachine : public MachineBase
{
public:
	static constexpr double kCentreY      = 0.06;
	static constexpr double kBaseY        = -0.28;
	static constexpr double kSparkSeconds = 1e-6;///< a spark's current pulse
	static constexpr double kGroundWeight = 20.0;
	/// The charging ODE's step. Backward Euler: exact for the belt alone (the
	/// voltage is linear in time), unconditionally stable against the corona's
	/// sub-millisecond time constant C / G.
	static constexpr double kSubstep = Engine::kVdgSubstep;

	/// The room's ground, for the corona current: the distance from the
	/// sphere's centre to the grounded base plate.
	double GroundDistance() const
	{
		return kCentreY - kBaseY;
	}
	double CoronaOnset() const
	{
		return std::clamp( s.finish, kRoughestFinish, 1.0 ) * breakdown;
	}
	double Conductance() const
	{
		return CoronaConductance( s.sphere, CoronaOnset(), GroundDistance() );
	}

	double B() const
	{
		return kVdgGroundRatio * s.sphere;
	}
	double Ax() const
	{
		return -( 0.5 * s.gap + s.sphere );
	}
	double Bx() const
	{
		return 0.5 * s.gap + B();
	}

	/// The spheres and a margin; past it the field is the room's, and a
	/// lattice over the whole 16:9 frame cost twice as much for nothing.
	double FieldWidth() const override
	{
		return 1.6 * H();
	}

	uint64_t PaintKey() const override
	{
		uint64_t k = 3;
		k          = Mix( k, s.sphere );
		k          = Mix( k, s.gap );
		return k ^ ClipKey();
	}

	void PaintApparatus() override
	{
		PaintDisc( Ax(), kCentreY, s.sphere, Paint::Source, 1.0f );
		PaintDisc( Bx(), kCentreY, B(), Paint::Ground, 0.0f );
		// Where a spark may START: breakdown begins where the surface field
		// first reaches Peek's value, which is the cap facing the gap (the
		// two-sphere field falls away from the axis). Without this one spark in
		// four sprouted from the back of the sphere and wandered a metre and a
		// half round the frame before finding ground -- 1000 sites, 86 ms.
		// Corona is not restricted: it glows from the whole sphere.
		{
			const double h = field.Step();
			sparkAllowed.assign( static_cast< size_t >( field.Nx() ) * field.Ny(), 1 );
			ForBox( Ax() - s.sphere - 3 * h, kCentreY - s.sphere - 3 * h, Ax() + s.sphere + 3 * h, kCentreY + s.sphere + 3 * h,
			        [ & ]( int cell, double x, double y ) {
				        const double dx = x - Ax(), dy = y - kCentreY, r = Hypot( dx, dy );
				        if( r > s.sphere && r <= s.sphere + 2.5 * h && dx < r * std::cos( 50.0 * kPi / 180.0 ) )
					        sparkAllowed[ static_cast< size_t >( cell ) ] = 0;
			        } );
		}
		PaintCapsule( Bx(), kCentreY, Bx(), kBaseY, 0.010, Paint::Ground, 0.0f );
		ForBox( -kHuge, -kHuge, kHuge, kBaseY, [ & ]( int cell, double, double ) { PaintCell( cell, Paint::Ground, 0.0f ); } );
	}

	void Configure() override
	{
		spheres   = SolveTwoSpheres( s.sphere, B(), s.gap );
		breakdown = VdgBreakdownVolts( spheres, s.sphere, B() );
	}

	void Restart( double t ) override
	{
		volts    = 0.0;
		lastTime = t;
		tree.Clear();
		corona.Clear();
		fire = false;
	}

	void Fire( double ) override
	{
		fire = true;
	}

	void Spark( double t, double v, bool exposed, int frame, Frame& out, std::vector< Event >& log )
	{
		tree.Clear();
		field.Adopt( tree );
		GrowthSettings g = Growth( s.eta, true );
		g.allowed        = &sparkAllowed;
		field.Grow( tree, 4000, kHuge, rng, t, g );
		Event e;
		e.time      = t;
		e.joules    = 0.5 * spheres.capacitance * v * v;
		e.connected = tree.Connected();
		e.exposed   = exposed;
		e.frame     = exposed ? frame : -1;
		e.length    = tree.LongestPath();
		e.nodes     = static_cast< int >( tree.Size() );
		tree.Currents( 1.0, kGroundWeight );
		for( const Node& n : tree.Nodes() )
			if( n.children == 0 && !n.grounded )
				++e.branches;
		if( exposed )
		{
			Emit( tree, spheres.capacitance * v / kSparkSeconds, kGroundWeight, e.joules * s.efficiency, out );
			e.light = e.joules * s.efficiency;
			++out.events;
		}
		log.push_back( e );
		// The spark is over in a microsecond: its channel is gone by the next
		// frame, and the field is clear for the corona.
		for( const Node& n : tree.Nodes() )
			field.Forget( n );
		tree.Clear();
		volts = 0.0;
	}

	/// The charging ODE,
	///
	///     C dV/dt = I_belt - G (V - V_c)   for V > V_c,   I_belt below it,
	///
	/// integrated here by backward Euler; `hvtest --vdg` holds it to the
	/// closed form. A polished sphere has V_c = V_b -- its corona onset IS the
	/// gap's breakdown, both being Peek's field at the facing point -- so the
	/// corona term never acts and the interval is C V_b / I exactly. A rough
	/// one coronas first, and since G is tens of nA per volt against a belt of
	/// microamps, the corona does not slow the climb so much as stop it: the
	/// sphere settles at V_c + I_belt / G within a millisecond. If that is
	/// below V_b it never sparks and glows instead.
	void Step( double t0, double t1, double exposeFrom, int frame, Frame& out, std::vector< Event >& log ) override
	{
		const double C  = spheres.capacitance;
		const double Vc = CoronaOnset();
		const double G  = Conductance();
		const double I  = std::max( s.belt, 0.0 );
		double coronaJoules = 0.0;
		double t            = t0;
		while( t < t1 - 1e-12 )
		{
			const double dt = std::min( kSubstep, t1 - t );
			double v        = volts + I * dt / C;
			if( v > Vc && Vc < breakdown && G > 0.0 )
				v = ( volts + dt / C * ( I + G * Vc ) ) / ( 1.0 + dt * G / C );
			if( v >= breakdown && v > volts )
			{
				// When in the step, linearly: exact for the belt alone.
				const double f  = ( breakdown - volts ) / ( v - volts );
				const double ts = t + f * dt;
				Spark( ts, breakdown, ts > exposeFrom, frame, out, log );
				volts = 0.0;
				t     = ts;
				continue;
			}
			const double ic = G * std::max( 0.0, v - Vc );
			coronaJoules += v * ic * Exposed( t, t + dt, exposeFrom );
			volts = v;
			t += dt;
		}
		coronaAmps = G * std::max( 0.0, volts - Vc );
		if( fire )
		{
			fire = false;
			if( volts > 0.2 * breakdown )
				Spark( t1, volts, true, frame, out, log );
		}

		// Corona: a tuft of eta = 1 growth, fresh each frame, as large as the
		// share of the belt's current it carries, and emitting exactly the power
		// it drained, V I_c, times the efficiency.
		const int sites = static_cast< int >( std::lround( 28.0 * s.reach * std::min( 1.0, coronaAmps / std::max( I, 1e-12 ) ) ) );
		corona.Clear();
		if( sites > 0 )
		{
			GrowthSettings g = Growth( 1.0, true );
			field.Grow( corona, sites, kHuge, rng, t1, g );
			Emit( corona, coronaAmps, kGroundWeight, coronaJoules * s.efficiency, out );
			for( const Node& n : corona.Nodes() )
				field.Forget( n );
		}
		lastTime = t1;
	}

	void Shapes( Frame& out ) const override
	{
		const float ax = static_cast< float >( Ax() ), bx = static_cast< float >( Bx() );
		const float y = static_cast< float >( kCentreY );
		out.shapes.push_back( { Shape::Box, -50.0f, -50.0f, 50.0f, static_cast< float >( kBaseY - 0.05 ), 0.0f, Shape::Floor } );
		out.shapes.push_back( { Shape::Box, static_cast< float >( Ax() - 0.2 ), static_cast< float >( kBaseY - 0.05 ), static_cast< float >( Bx() + 0.1 ),
		                        static_cast< float >( kBaseY ), 0.0f, Shape::Dull } );
		out.shapes.push_back( { Shape::Capsule, ax, y, ax, static_cast< float >( kBaseY ), static_cast< float >( 0.30 * s.sphere ), Shape::Acrylic } );
		out.shapes.push_back( { Shape::Capsule, bx, y, bx, static_cast< float >( kBaseY ), 0.008f, Shape::Metal } );
		out.shapes.push_back( { Shape::Disc, ax, y, 0, 0, static_cast< float >( s.sphere ), Shape::Metal } );
		out.shapes.push_back( { Shape::Disc, bx, y, 0, 0, static_cast< float >( B() ), Shape::Metal } );
	}

	TwoSpheres spheres;
	double breakdown  = 1.0;
	double volts      = 0.0;
	double coronaAmps = 0.0;
	double lastTime  = 0.0;
	bool fire        = false;
	Tree corona;
	std::vector< uint8_t > sparkAllowed;
};

//===========================================================================
// Plasma globe.
//===========================================================================
class GlobeMachine : public MachineBase
{
public:
	static constexpr double kCentreY      = 0.03;
	static constexpr double kElectrode    = 0.12;///< of the glass radius
	static constexpr float kGlassPotential = 0.35f;///< the glass couples weakly to the room
	static constexpr double kFingerRadius = 0.025;
	static constexpr double kGroundWeight = 40.0;

	double FieldWidth() const override
	{
		return H();
	}

	uint64_t PaintKey() const override
	{
		uint64_t k = 4;
		k          = Mix( k, s.globe );
		k          = Mix( k, s.finger ? 1.0 : 0.0 );
		const auto f = Finger();
		k          = Mix( k, f.first );
		k          = Mix( k, f.second );
		return k ^ ClipKey();
	}

	/// Where the finger touches: the point of the glass nearest to Finger X/Y,
	/// or in Over, nearest to the brightest thing in the clip.
	std::pair< double, double > Finger() const
	{
		double fx, fy;
		FrameToScene( s.fingerX, s.fingerY, fx, fy );
		if( s.clip != nullptr && s.clipW > 0 )
		{
			// The clip's bright cells, averaged: its brightest region.
			double sx = 0, sy = 0, n = 0;
			for( int j = 0; j < s.clipH; ++j )
				for( int i = 0; i < s.clipW; ++i )
					if( ( *s.clip )[ static_cast< size_t >( j * s.clipW + i ) ] )
					{
						sx += ( i + 0.5 ) / s.clipW;
						sy += ( j + 0.5 ) / s.clipH;
						n += 1;
					}
			if( n > 0 )
				FrameToScene( sx / n, sy / n, fx, fy );
		}
		const double dx = fx, dy = fy - kCentreY;
		const double d  = std::max( Hypot( dx, dy ), 1e-6 );
		return { dx / d * s.globe, kCentreY + dy / d * s.globe };
	}

	void PaintApparatus() override
	{
		const double h     = field.Step();
		const double thick = std::max( 0.004, 1.6 * h );
		const auto finger  = Finger();
		ForBox( -s.globe - thick, kCentreY - s.globe - thick, s.globe + thick, kCentreY + s.globe + thick,
		        [ & ]( int cell, double x, double y ) {
			        const double r = Hypot( x, y - kCentreY );
			        if( r < s.globe || r > s.globe + thick )
				        return;
			        const bool touched = s.finger && Hypot( x - finger.first, y - finger.second ) <= kFingerRadius + h;
			        PaintCell( cell, Paint::Ground, touched ? 0.0f : kGlassPotential );
		        } );
		PaintDisc( 0.0, kCentreY, kElectrode * s.globe, Paint::Source, 1.0f );
		// The stem carries the drive up to the electrode: a conductor at its
		// potential, inside a glass sleeve, so nothing grows from it.
		PaintCapsule( 0.0, kCentreY - kElectrode * s.globe, 0.0, kCentreY - s.globe, 0.006, Paint::Held, 1.0f );
	}

	int Filaments() const
	{
		// More drive, more filaments: 2 + one per 3.5 kV, so the nominal 25 kV
		// flyback lights nine.
		return std::clamp( static_cast< int >( std::lround( 2.0 + s.supply.openVolts / 3500.0 ) ), 3, 14 );
	}

	void Restart( double t ) override
	{
		tree.Clear();
		field.Adopt( tree );
		lastT = t;
	}

	void Fire( double ) override
	{
		surge = true;
	}

	void Step( double t0, double t1, double exposeFrom, int frame, Frame& out, std::vector< Event >& log ) override
	{
		( void )frame;
		( void )log;
		std::vector< Node >& nodes = tree.Nodes();
		if( surge )
		{
			surge = false;
			tree.Clear();
		}
		else if( !nodes.empty() )
		{
			// A filament that carries a circuit stays; a dead end does not. The
			// RF re-strikes the globe every cycle, so a branch that never
			// reached the glass is gone by the next frame. (Keeping dead ends for
			// tau, as the coil keeps its streamers, grew a bush of them against
			// the glass on whichever side the first filaments landed.)
			std::vector< uint8_t > keep( nodes.size(), 0 );
			for( size_t i = 0; i < nodes.size(); ++i )
				if( nodes[ i ].grounded )
					for( int n = static_cast< int >( i ); n >= 0; n = nodes[ static_cast< size_t >( n ) ].parent )
						keep[ static_cast< size_t >( n ) ] = 1;
			// Drift: each filament re-forms its outer part every frame, a
			// fraction 1 - exp(-dt / tau) of it at most -- the same channel
			// memory as the coil's, at the rate the RF re-strikes it.
			const double f = 1.0 - std::exp( -( t1 - t0 ) / std::max( s.memory, 1e-4 ) );
			for( size_t i = 0; i < nodes.size(); ++i )
			{
				if( !nodes[ i ].grounded )
					continue;
				std::vector< int > path;
				for( int n = static_cast< int >( i ); n >= 0; n = nodes[ static_cast< size_t >( n ) ].parent )
					path.push_back( n );
				const double cut = f * Unit( rng.Next() );
				const size_t drop = std::max< size_t >( 1, static_cast< size_t >( std::ceil( cut * path.size() ) ) );
				for( size_t k = 0; k < drop && k < path.size(); ++k )
					keep[ static_cast< size_t >( path[ k ] ) ] = 0;
			}
			for( size_t i = 0; i < nodes.size(); ++i )
			{
				nodes[ i ].alive    = keep[ i ] != 0;
				nodes[ i ].grounded = false;
			}
			tree.Compact();
		}
		field.Adopt( tree );
		tree.Compact();
		field.Adopt( tree );

		// Filaments one at a time: each burst grows until one path reaches the
		// glass, that path becomes an arc (a resistor, no longer a source), and
		// the dead ends the burst left are dropped -- the RF re-strikes along
		// the path that made it, not the ones that did not. Grown all at once,
		// the dead branches beside the first contact sat in the strongest field
		// in the globe (a conductor at 1 a site from glass at 0.35) and spent
		// the whole budget crawling along the glass.
		const int want = Filaments();
		const int cap  = static_cast< int >( 160 * s.reach );
		for( int f = 0; f < want; ++f )
		{
			const size_t before = tree.Size();
			GrowthSettings g    = Growth( s.eta, true );
			const GrowthResult r = field.Grow( tree, cap, kHuge, rng, t1, g );
			std::vector< Node >& grown = tree.Nodes();
			if( !r.reachedGround )
			{
				for( size_t i = before; i < grown.size(); ++i )
				{
					grown[ i ].alive = false;
					field.Forget( grown[ i ] );
				}
				tree.Compact();
				break;
			}
			std::vector< uint8_t > onPath( grown.size(), 0 );
			for( int n = r.groundNode; n >= 0; n = grown[ static_cast< size_t >( n ) ].parent )
				onPath[ static_cast< size_t >( n ) ] = 1;
			for( size_t i = before; i < grown.size(); ++i )
				if( !onPath[ i ] )
				{
					grown[ i ].alive = false;
					field.Forget( grown[ i ] );
				}
			tree.Compact();
			field.Adopt( tree );
			// Every connected path is an arc again after Adopt.
			for( size_t i = 0; i < tree.Nodes().size(); ++i )
				if( tree.Nodes()[ i ].grounded )
					field.Conduct( tree, static_cast< int >( i ), kGlassPotential );
		}
		const double power = s.supply.MaxPower();
		Emit( tree, power / s.supply.openVolts, kGroundWeight, power * Exposed( t0, t1, exposeFrom ) * s.efficiency, out );
		if( Exposed( t0, t1, exposeFrom ) > 0.0 )
			++out.events;
		lastT = t1;
	}

	void Shapes( Frame& out ) const override
	{
		const float g = static_cast< float >( s.globe );
		const float y = static_cast< float >( kCentreY );
		out.shapes.push_back( { Shape::Box, -0.55f * g, y - 1.40f * g, 0.55f * g, y - 0.94f * g, 0.0f, Shape::Dull } );
		out.shapes.push_back( { Shape::Capsule, 0.0f, y - static_cast< float >( kElectrode ) * g, 0.0f, y - g, 0.012f, Shape::Glass } );
		out.shapes.push_back( { Shape::Disc, 0.0f, y, 0, 0, static_cast< float >( kElectrode ) * g, Shape::Glass } );
		out.shapes.push_back( { Shape::Ring, 0.0f, y, 0.004f, 0, g, Shape::Glass } );
	}

	bool surge  = false;
	double lastT = 0.0;
};

//===========================================================================
// Lichtenberg figure.
//===========================================================================
class LichtenbergMachine : public MachineBase
{
public:
	static constexpr double kHalf        = 0.165;///< half the slab, metres
	static constexpr double kGrowSeconds = 3.0;  ///< the staging: how long the figure takes to grow
	static constexpr int kSites          = 1600; ///< nominal budget at Reach 1
	static constexpr double kGroundWeight = 20.0;

	double FieldWidth() const override
	{
		return H();
	}

	uint64_t PaintKey() const override
	{
		uint64_t k = 5;
		k          = Mix( k, s.origin );
		return k ^ ClipKey();
	}

	void PaintApparatus() override
	{
		const double h = field.Step();
		if( s.origin == 0 )
		{
			PaintDisc( 0.0, 0.0, 1.5 * h, Paint::Source, 1.0f );
			// Ground all round, a circle, so the figure grows the same way in
			// every direction until it nears the edge.
			ForBox( -kHuge, -kHuge, kHuge, kHuge, [ & ]( int cell, double x, double y ) {
				if( Hypot( x, y ) >= kHalf )
					PaintCell( cell, Paint::Ground, 0.0f );
			} );
		}
		else
		{
			// From the bottom edge up toward the top: a uniform field, with the
			// slab's sides held at it so it stays uniform.
			ForBox( -kHuge, -kHuge, kHuge, -kHalf, [ & ]( int cell, double x, double ) {
				if( std::fabs( x ) <= kHalf )
					PaintCell( cell, Paint::Source, 1.0f );
			} );
			ForBox( -kHuge, kHalf, kHuge, kHuge, [ & ]( int cell, double, double ) { PaintCell( cell, Paint::Ground, 0.0f ); } );
			ForBox( -kHuge, -kHalf, kHuge, kHalf, [ & ]( int cell, double x, double y ) {
				if( std::fabs( x ) > kHalf )
					PaintCell( cell, Paint::Held, static_cast< float >( 0.5 - 0.5 * y / kHalf ) );
			} );
		}
	}

	int Budget() const
	{
		return static_cast< int >( std::lround( kSites * s.reach ) );
	}

	void Restart( double t ) override
	{
		tree.Clear();
		field.Adopt( tree );
		started = t;
	}

	void Fire( double t ) override
	{
		restartAt = t;
	}

	void Step( double t0, double t1, double exposeFrom, int frame, Frame& out, std::vector< Event >& log ) override
	{
		if( restartAt > -kHuge )
		{
			Restart( restartAt );
			restartAt = -kHuge;
			Event e;
			e.time    = t1;
			e.exposed = true;
			e.frame   = frame;
			log.push_back( e );
		}
		const int want = std::min( Budget(), static_cast< int >( std::ceil( Budget() * ( t1 - started ) / kGrowSeconds ) ) );
		const int have = static_cast< int >( tree.Size() );
		if( want > have && !tree.Connected() )
		{
			GrowthSettings g = Growth( s.eta, true );
			field.Grow( tree, want - have, kHuge, rng, t1, g );
		}
		const double power = s.supply.MaxPower();
		Emit( tree, s.supply.ShortCircuitAmps(), kGroundWeight, power * Exposed( t0, t1, exposeFrom ) * s.efficiency, out );
		if( Exposed( t0, t1, exposeFrom ) > 0.0 && !tree.Empty() )
			++out.events;
	}

	void Shapes( Frame& out ) const override
	{
		const float k = static_cast< float >( kHalf ) + 0.012f;
		out.shapes.push_back( { Shape::Box, -k, -k, k, k, 0.0f, Shape::Acrylic } );
		if( s.origin == 0 )
			out.shapes.push_back( { Shape::Disc, 0.0f, 0.0f, 0, 0, 0.004f, Shape::Metal } );
		else
			out.shapes.push_back( { Shape::Box, -k, -k - 0.006f, k, -k, 0.0f, Shape::Metal } );
	}

	double started   = 0.0;
	double restartAt = -kHuge;
};

//===========================================================================
// The engine.
//===========================================================================
class Engine::Impl
{
public:
	Impl()
	{
		machines[ static_cast< int >( Machine::Ladder ) ]      = std::make_unique< LadderMachine >();
		machines[ static_cast< int >( Machine::Tesla ) ]       = std::make_unique< TeslaMachine >();
		machines[ static_cast< int >( Machine::VanDeGraaff ) ] = std::make_unique< VdgMachine >();
		machines[ static_cast< int >( Machine::Globe ) ]       = std::make_unique< GlobeMachine >();
		machines[ static_cast< int >( Machine::Lichtenberg ) ] = std::make_unique< LichtenbergMachine >();
	}

	MachineBase& Active()
	{
		return *machines[ static_cast< int >( active ) ];
	}

	std::unique_ptr< MachineBase > machines[ static_cast< int >( Machine::Count ) ];
	Machine active   = Machine::Count;
	double clock     = -1.0;///< engine seconds of the last Advance, -1 before the first
	int frame        = 0;
	bool fire        = false;
	bool restart     = true;
};

Engine::Engine() :
	impl( std::make_unique< Impl >() )
{
}

Engine::~Engine() = default;

void Engine::Configure( const Settings& next )
{
	settings = next;
	const Machine m = std::clamp( next.machine, Machine::Ladder, Machine::Lichtenberg );
	if( m != impl->active )
	{
		impl->active  = m;
		impl->restart = true;
	}
	impl->Active().Setup( settings );
}

void Engine::Fire()
{
	impl->fire = true;
}

void Engine::Restart()
{
	impl->restart = true;
}

void Engine::Advance( double now, double framePeriod, Frame& out )
{
	const auto start = std::chrono::steady_clock::now();
	out.segments.clear();
	out.shapes.clear();
	out.joules = 0.0;
	out.events = 0;

	MachineBase& m  = impl->Active();
	out.sceneHeight = SceneHeight( impl->active );

	// A jump -- backwards, or more than half a second forwards -- is not
	// replayed: re-anchor, keep the discharges, carry on.
	if( impl->clock < 0.0 || now < impl->clock || now - impl->clock > 0.5 || impl->restart )
	{
		if( impl->restart || impl->clock < 0.0 )
			m.Restart( now );
		else
			m.Rebase( now );
		impl->restart = false;
		impl->clock   = now;
		if( impl->fire )
		{
			impl->fire = false;
			m.Fire( now );
		}
		m.Shapes( out );
		++impl->frame;
		lastMillis = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count();
		return;
	}

	if( impl->fire )
	{
		impl->fire = false;
		m.Fire( now );
	}

	const double period     = std::max( framePeriod, now - impl->clock );
	const double exposeFrom = now - std::clamp( settings.shutter, 0.0, 1.0 ) * period;
	m.Step( impl->clock, now, exposeFrom, impl->frame, out, events );
	// The plugin never reads the log; the harness clears it. Either way it
	// must not grow for the length of a show.
	if( events.size() > 20000 )
		events.erase( events.begin(), events.begin() + 10000 );
	m.Shapes( out );
	impl->clock = now;
	++impl->frame;
	lastMillis = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count();
}

const Tree& Engine::CurrentTree() const
{
	return impl->machines[ static_cast< int >( impl->active ) ]->tree;
}

const Tree& Engine::LastEmitted() const
{
	return impl->machines[ static_cast< int >( impl->active ) ]->lastEmitted;
}

Field& Engine::CurrentField()
{
	return impl->Active().field;
}

LadderProbe& Engine::Ladder()
{
	return static_cast< LadderMachine& >( *impl->machines[ static_cast< int >( Machine::Ladder ) ] ).probe;
}

double Engine::VdgBreakdown() const
{
	return static_cast< const VdgMachine& >( *impl->machines[ static_cast< int >( Machine::VanDeGraaff ) ] ).breakdown;
}

double Engine::VdgCapacitance() const
{
	return static_cast< const VdgMachine& >( *impl->machines[ static_cast< int >( Machine::VanDeGraaff ) ] ).spheres.capacitance;
}

namespace
{
const VdgMachine& Vdg( const std::unique_ptr< MachineBase >* machines )
{
	return static_cast< const VdgMachine& >( *machines[ static_cast< int >( Machine::VanDeGraaff ) ] );
}
} // namespace

double Engine::VdgVolts() const
{
	return Vdg( impl->machines ).volts;
}

double Engine::VdgCoronaOnset() const
{
	return Vdg( impl->machines ).CoronaOnset();
}

double Engine::VdgConductance() const
{
	return Vdg( impl->machines ).Conductance();
}

double Engine::VdgGroundDistance() const
{
	return Vdg( impl->machines ).GroundDistance();
}

double Engine::TeslaBangJoules() const
{
	return static_cast< const TeslaMachine& >( *impl->machines[ static_cast< int >( Machine::Tesla ) ] ).BangEnergy();
}
} // namespace flyback

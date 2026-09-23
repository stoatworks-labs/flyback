#pragma once

#include "Lattice.h"
#include "Rng.h"
#include "Tree.h"

#include <cstdint>
#include <vector>

/**
    The dielectric breakdown model (Niemeyer, Pietronero & Wiesmann, PRL 52,
    1033, 1984).

    The electrodes and the channel already grown are held at their potential,
    the potential phi in the air solves Laplace's equation (Lattice), and the
    channel extends to one neighbouring site at a time with probability

        p  ~  |grad phi| ^ eta

    where the gradient is taken along the bond from the channel to the
    candidate: (V_channel - phi(candidate)) / bond length.

    That one exponent decides the look. eta = 1 is DLA's universality class --
    a Lichtenberg figure, fractal dimension 1.71 in the plane -- eta = 2 to 3
    is lightning, and a large eta is a nearly straight arc. Branching, forking
    toward the nearest ground, streamers bending toward each other and a bolt
    taking the short way round an obstacle are not drawn: they are where the
    field is.

    ## Growth, one site at a time, and what is approximated

    Each site is chosen from the field as it stands. Between full solves the
    engine does not re-solve the whole lattice after every site: it relaxes a
    window around the new site (`relaxRadius`, `relaxSweeps` Gauss-Seidel
    sweeps) so the field near the tip that just moved is right, and it solves
    the whole lattice again, to `tolerance`, every `refreshEvery` sites and at
    the start of every burst. The error that leaves is the far-field part of
    the last few sites' influence. It was measured while choosing the numbers
    (AGENTS.md): at eta = 1 the fractal dimension is 1.74 +- 0.03 with a full
    solve per site and 1.71 +- 0.03 with this scheme, over four seeds each --
    the same, and 23 times cheaper. `hvtest --dimension` runs the scheme that
    ships.

    ## Neighbours

    Growth is to the eight neighbours of the channel (diagonal bonds are
    sqrt 2 long and their field is divided accordingly), so a channel is not
    confined to the lattice's two axes. The Laplace stencil stays five-point.
*/
namespace flyback
{
enum class Cell : uint8_t
{
	Free,
	Source, ///< an electrode the discharge grows FROM (held at 1)
	Ground, ///< a conductor it grows TO (held at its own potential); touching it completes the circuit
	Channel,///< grown (held at 1)
	Arc,    ///< grown, and carrying a completed circuit: held at its own potential, no longer a growth source
	Held,   ///< an inert conductor at a fixed potential: neither a source nor a ground (a coil, the idle part of a rod)
	Wall    ///< the lattice's outer ring
};

struct GrowthSettings
{
	double eta          = 2.0;
	int refreshEvery    = 32;
	int relaxRadius     = 8;
	int relaxSweeps     = 4;
	float tolerance     = 1e-5f;
	bool stopAtGround   = true;///< a completed circuit ends the burst
	bool diagonal       = true;///< grow to the 8 neighbours (false: the 4 bonds only)

	/// With stopAtGround off, a tip that reaches ground turns its path into an
	/// arc (Conduct) and growth carries on elsewhere, until this many circuits
	/// are complete. The plasma globe's filaments.
	int maxConnections = 1 << 30;

	/// If set, only cells with a non-zero entry may be grown into -- the
	/// Jacob's ladder's hot column, which the channel is re-grown inside
	/// every frame.
	const std::vector< uint8_t >* allowed = nullptr;

	/// Air is not uniform. Dust, humidity and the density fluctuations of warm
	/// air make the field needed to break it vary from place to place, and it
	/// is that, not the lattice, that should decide which way a streamer
	/// wanders. Each site's growth weight is multiplied by exp( disorder xi ),
	/// xi a standard normal fixed per site for the burst (`disorderKey`). Without
	/// it the square lattice imposes its own four axes on anything with
	/// eta above about 2 -- the "plus sign" the first Tesla coil render showed.
	/// Quenched disorder in the breakdown threshold is a known extension of
	/// the model; the harness measures the dimension WITH it, as shipped.
	float disorder       = 0.5f;
	uint32_t disorderKey = 0;

	/// Optional bias on growth: a candidate's weight is multiplied by
	/// exp( biasX dx + biasY dy ) for a bond (dx, dy). Zero by default.
	float biasX = 0.0f, biasY = 0.0f;
};

struct GrowthResult
{
	int grown           = 0;
	double length       = 0.0;///< metres of new channel
	bool reachedGround  = false;
	int groundNode      = -1;
	int connections     = 0;
};

/// The world one machine's discharge grows in: a lattice sized in metres, and
/// what every cell of it is.
class Field
{
public:
	/// `cellsHigh` across the scene's height of `heightMetres`; the width
	/// follows the aspect. The lattice is rounded up to a size multigrid can
	/// coarsen, and centred on the scene.
	bool Reset( double widthMetres, double heightMetres, int cellsHigh, float ringPotential );

	Lattice& Grid()
	{
		return lattice;
	}
	const Lattice& Grid() const
	{
		return lattice;
	}

	int Nx() const
	{
		return lattice.Nx();
	}
	int Ny() const
	{
		return lattice.Ny();
	}
	double Step() const
	{
		return h;
	}
	double X( int i ) const
	{
		return x0 + h * i;
	}
	double Y( int j ) const
	{
		return y0 + h * j;
	}

	/// The nearest lattice site to a scene point, or -1 if it is outside.
	int CellAt( double x, double y ) const;

	Cell Kind( int index ) const
	{
		return kind[ static_cast< size_t >( index ) ];
	}
	int NodeAt( int index ) const
	{
		return nodeAt[ static_cast< size_t >( index ) ];
	}

	/// Paint a conductor. Painting over a channel cell is refused.
	void PaintSource( int index );
	void PaintGround( int index, float potential );
	void PaintHeld( int index, float potential );

	/// Everything that is not the ring, back to free air.
	void ClearConductors();

	/// Put a tree's nodes into the lattice as channel. Nodes that land on a
	/// painted conductor are dropped (the tree must be compacted after).
	void Adopt( Tree& tree );

	/// Take a node's cell back to free air (a channel that has cooled).
	void Forget( const Node& node );

	/// The path from `tip` to its root has completed a circuit: it becomes an
	/// arc, a resistor, held at a potential falling linearly along it from 1 at
	/// the electrode to `endPotential` at the ground, and it no longer grows.
	void Conduct( Tree& tree, int tip, float endPotential );

	/// Seed every free cell with a guess, for a better first solve.
	void GuessAll( float value );

	/// Grow at most `maxSites` sites, or `lengthBudget` metres of channel,
	/// whichever runs out first.
	GrowthResult Grow( Tree& tree, int maxSites, double lengthBudget, Pcg32& rng, double now,
	                   const GrowthSettings& settings );

	/// Candidates in the last burst -- for the bench.
	size_t LastCandidates() const
	{
		return lastCandidates;
	}

private:
	void Rebuild( const GrowthSettings& settings );
	void AddCandidate( int cell );
	void RemoveCandidate( int cell );
	double WeightOf( int cell, const GrowthSettings& settings, int& parentCell ) const;
	void SetWeight( int slot, double weight );
	int Pick( double u ) const;
	bool TouchesGround( int cell, int& groundCell ) const;

	Lattice lattice;
	std::vector< Cell > kind;
	std::vector< int > nodeAt;
	double h = 1.0, x0 = 0.0, y0 = 0.0;

	// Candidates: a slot per candidate, a Fenwick tree over the slots' weights.
	std::vector< int > slotOf;   ///< cell -> slot, -1 if not a candidate
	std::vector< int > cellOf;   ///< slot -> cell, -1 if free
	std::vector< double > weight;///< slot -> weight
	std::vector< double > fenwick;
	std::vector< int > freeSlots;
	size_t lastCandidates = 0;
};
} // namespace flyback

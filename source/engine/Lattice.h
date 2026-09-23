#pragma once

#include <cstdint>
#include <vector>

/**
    The potential in the air, on a lattice: a real Laplace solve.

    Every discharge in the plugin grows where the field is strongest, so this
    is the one piece of numerics everything else stands on. It solves the
    five-point discrete Laplace equation

        phi(i-1,j) + phi(i+1,j) + phi(i,j-1) + phi(i,j+1) - 4 phi(i,j) = 0

    on every free cell, with Dirichlet cells held at their values: the
    electrodes, the grounds, the channel already grown, and the lattice's own
    outer ring.

    ## Why not the point-charge superposition the spec suggested

    Kim, Sewall & Lin's method treats every channel site as an equal point
    charge and sums `ln r`. It is O(candidates) per step and needs no solve --
    and it is not the dielectric breakdown model. A conductor is an
    equipotential, and its charge piles up at the tips; equal charges put too
    much of it in the fjords. Measured on this machine during the build, at
    eta = 1 it grows clusters whose mass dimension scatters from 1.67 to 2.11
    between seeds, and its min/max normalisation pushes the mean to 2.19. DLA's
    is 1.71. So the potential is solved, and AGENTS.md has the numbers.

    ## How

    Preconditioned conjugate gradients, with one multigrid V-cycle as the
    preconditioner. Multigrid on its own converges at 0.03 a cycle on an empty
    rectangle and at only 0.4 to 0.6 a cycle once a spidery conductor fills the
    interior, because a coarse grid cannot represent a wire one cell wide. CG
    mops up exactly the few modes that the V-cycle gets wrong, and needs about
    five iterations from a warm start.

    The V-cycle: red-black Gauss-Seidel, two sweeps each way, in opposite
    colour order on the way up so the preconditioner is symmetric (CG needs it
    to be); full weighting down, bilinear up; a coarse cell is held (its
    correction is zero) when any fine cell under its 3x3 stencil is held.

    The lattice must be `a * 2^k + 1` cells on each side so it coarsens
    exactly; `GoodSize` rounds up to that.

    ## Units

    The residual is the five-point Laplacian with unit spacing, so a
    tolerance is in units of potential (the conductor sits at 1). The lattice
    step in metres is the caller's business: this class never sees a metre,
    which is what keeps it testable against the coaxial closed form on its own.
*/
namespace flyback
{
class Lattice
{
public:
	/// Round `wanted` up to the nearest `a * 2^levels + 1` with a >= 2.
	static int GoodSize( int wanted, int levels );

	/// Allocate. Both sizes must coarsen at least twice. Everything starts
	/// free at potential 0, except the outer ring, which is held at 0.
	bool Resize( int nx, int ny );

	int Nx() const
	{
		return nx;
	}
	int Ny() const
	{
		return ny;
	}
	int Index( int i, int j ) const
	{
		return j * nx + i;
	}

	/// Hold a cell at a potential. Cheap: the coarse masks are updated
	/// incrementally.
	void Hold( int index, float value );

	/// Release a held cell. Marks the coarse masks for a rebuild, which
	/// happens at the next Solve.
	void Release( int index );

	/// Release everything except the outer ring, and put every free cell back
	/// to `guess`.
	void ReleaseAll( float guess );

	bool Held( int index ) const
	{
		return held[ static_cast< size_t >( index ) ] != 0;
	}

	float Potential( int index ) const
	{
		return phi[ static_cast< size_t >( index ) ];
	}

	/// Set a free cell's value (a starting guess). Ignored for held cells.
	void Guess( int index, float value );

	/// Solve to `tolerance` (max |residual|, unit spacing). Returns the number
	/// of CG iterations, or -1 if it did not converge in `maxIterations`.
	int Solve( float tolerance, int maxIterations = 200 );

	/// Gauss-Seidel on the free cells within `radius` of (i, j). The engine
	/// calls it after each site it adds, so the field near the newest tip is
	/// right before the next choice even between full solves.
	void Relax( int i, int j, int radius, int sweeps );

	/// max |residual| over the free cells, unit spacing.
	float Residual() const;

	/// How many full solves and CG iterations since construction.
	long Solves() const
	{
		return solves;
	}
	long Iterations() const
	{
		return iterations;
	}

private:
	struct Level
	{
		int nx = 0, ny = 0;
		std::vector< float > u, f, r;
		std::vector< uint8_t > held;
		std::vector< float > free;///< 1 - held, as a float, for branchless loops
	};

	void BuildMasks();
	void MarkCoarse( int level, int i, int j );
	void Smooth( Level& level, int sweeps, bool redFirst ) const;
	void ResidualOf( Level& level ) const;
	void Restrict( const Level& fine, Level& coarse ) const;
	void ProlongAdd( const Level& coarse, Level& fine );
	void VCycle( size_t k );
	void Precondition( const std::vector< float >& in, std::vector< float >& out );
	void ApplyA( const std::vector< float >& x, std::vector< float >& y ) const;

	int nx = 0, ny = 0;
	std::vector< float > phi;
	std::vector< uint8_t > held;
	std::vector< Level > levels;
	bool masksDirty = true;

	std::vector< float > r, z, p, ap;
	std::vector< float > rowScratch;

	long solves     = 0;
	long iterations = 0;
};
} // namespace flyback

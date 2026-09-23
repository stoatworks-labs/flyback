#pragma once

#include <cstdint>
#include <vector>

/**
    A grown discharge: a forest of channels, each rooted on an electrode.

    Every node is one lattice site the breakdown model added, with a pointer to
    the site it grew from. A root grew straight off an electrode and remembers
    where on the electrode it attached.

    ## Current, by Kirchhoff

    The current into any node is the sum of the currents out of it. A node's
    own outflow is its leaf weight: an unconnected tip sheds the displacement
    current that charges the air ahead of it, and every such tip sheds the
    same (`kTipWeight`); a tip that has reached ground carries the discharge
    itself (`groundWeight`, the machine's choice); an interior node sheds
    nothing. So the current at a node is `unit * (sum of the leaf weights
    below it)`, the trunk carries the most, and the sums are integers held in
    doubles -- which is why `--kirchhoff` can demand float precision.

    ## Radius and light, from the current

    - **Radius goes as sqrt(I).** The channel's current density is roughly
      fixed by its temperature, so its cross-section scales with the current
      it carries.
    - **Emission per unit length goes as I^2 R' = I.** Resistance per unit
      length goes as 1/area, so as 1/I, and the power dissipated per metre is
      I^2 / I. A segment's share of the event's light is therefore
      proportional to I * length, normalised so the tree as a whole emits
      exactly the event's energy times the efficiency. Ten branches do not make
      ten times the light; they split it.
*/
namespace flyback
{
struct Node
{
	int cell   = -1;///< lattice index
	int parent = -1;///< node index; -1 for a root on an electrode
	float x = 0, y = 0;///< metres, scene coordinates (the lattice site)
	float ax = 0, ay = 0;///< a root's attachment point on its electrode, metres
	double born = 0.0;///< engine seconds when it grew
	double lastHot = 0.0;///< engine seconds when current last flowed through it
	int root = -1;///< the root node of its channel

	// Filled by Tree::Currents().
	double weight   = 0.0;///< sum of leaf weights at and below this node
	float current   = 0.0f;///< amperes
	int children    = 0;
	bool grounded   = false;///< this tip touched ground
	float gx = 0, gy = 0;///< where, metres (a ground contact point)
	bool alive      = true;
};

class Tree
{
public:
	static constexpr double kTipWeight = 1.0;

	void Clear()
	{
		nodes.clear();
	}
	bool Empty() const
	{
		return nodes.empty();
	}
	size_t Size() const
	{
		return nodes.size();
	}

	std::vector< Node >& Nodes()
	{
		return nodes;
	}
	const std::vector< Node >& Nodes() const
	{
		return nodes;
	}

	int Add( const Node& node );

	/// Kirchhoff: fill weight, current and children for every live node.
	/// `total` is the current into all the roots together, in amperes, and
	/// `groundWeight` how many tip-currents a grounded tip carries.
	void Currents( double total, double groundWeight );

	/// The largest |in - out| relative to the node's own current, over every
	/// live node, after Currents(). What `--kirchhoff` measures.
	double WorstKirchhoff() const;

	/// The spread of the currents at the unconnected tips: (max-min)/max.
	double TipSpread() const;

	/// Length of the longest root-to-tip path, metres.
	double LongestPath() const;

	/// Total length of all live segments, metres (roots measured from their
	/// attachment point).
	double TotalLength() const;

	/// Remove dead nodes and renumber. Parents must be alive for children to
	/// be kept; a child of a removed node is removed too.
	void Compact();

	/// True if any live tip reached ground.
	bool Connected() const;

private:
	std::vector< Node > nodes;
	double lastUnit         = 0.0;
	double lastGroundWeight = 1.0;
};
} // namespace flyback

//***************************************************************************
//
//	open_heap.h -- the open list of an A* solve: a binary min-heap owned by the solver that fills it
//
//	MechCommander II
//
//***************************************************************************

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "mc2fmt.h"

// One entry of an open list: the sort key, the id of the map position it stands for, and the row and column the solver reads back when it pops it.
struct PQNode
{
	int32_t key;
	uint32_t id;
	int32_t row;
	int32_t col;
};

namespace mc2
{

// WHAT: a binary min-heap of PQNode ordered by key (smallest first), with lookup by node id and re-keying of a node that is already in it.
// WHY it exists: it replaces PriorityQueue, a fixed 5002-node buffer shared by every solver through the global ::openList. Each solver (GlobalMap, MoveMap, the
// goal-map search) now owns its heap, so one solve can never read nodes another solve left behind, and the heap grows with the graph instead of dropping
// nodes past 5001.
//
// The tie rules are the old class's, unchanged, because they decide which of several equally good paths a unit takes (tests/unit/test_pqueue_contract.cpp
// pins them with 2000 recorded scripts): the NEWEST of equal keys rises above an equal parent, a sifting node stops on an equal key, and of two children
// with equal keys the left one wins.
//
// The index contract: a heap POSITION is only meaningful between the call that returned it and the next change to the heap. Solvers never hold one: they
// call rekey(id, key), which finds the node and moves it in one step. changeAt(position, key) exists for that one step and for the contract tests.
// Position 0 is a filler slot, never a node.
class OpenHeap
{
public:
	// `expectedNodes` sizes the first allocation to the graph (cells of a move map, doors of the global map); the heap grows past it when a search needs more,
	// and says so once per heap object: an upper bound that proved too small is a sizing mistake worth one log line, not a failure.
	explicit OpenHeap(int expectedNodes = 0)
	{
		nodes_.reserve(static_cast<std::size_t>(expectedNodes) + 2);
		nodes_.push_back(PQNode{0, 0xFFFFFFFFu, 0, 0}); // slot 0 only keeps the positions 1-based
	}

	void reserve(int expectedNodes) {
		const auto capacity = nodes_.capacity();
		nodes_.reserve(static_cast<std::size_t>(expectedNodes) + 2);
		allocations_ += nodes_.capacity() != capacity;
	}
	void clear() { nodes_.resize(1); }
	bool isEmpty() const { return nodes_.size() == 1; }
	int size() const { return static_cast<int>(nodes_.size()) - 1; }

	void insert(const PQNode& item)
	{
		if (nodes_.size() == nodes_.capacity()) {
			++allocations_;
			if (++growths_ == 1)
				mc2::fmt::print_err("[PATH v1] event=open_list_growth reserved={:d} needed={:d}\n", mc2::fmt::as_i32(static_cast<long>(nodes_.capacity())),
					mc2::fmt::as_i32(static_cast<long>(nodes_.size()) + 1));
		}
		nodes_.push_back(item);
		upHeap(size());
	}

	// Removes the node with the smallest key; an empty heap has no result.
	std::optional<PQNode> popMin()
	{
		if (isEmpty())
			return std::nullopt;
		const PQNode result = nodes_[1];
		nodes_[1] = nodes_.back();
		nodes_.pop_back();
		if (size() > 0)
			downHeap(1);
		return result;
	}

	// Position of the first node with this id, or 0 when the heap does not hold it.
	int find(uint32_t id) const
	{
		for (int position = 1; position <= size(); ++position)
			if (nodes_[static_cast<std::size_t>(position)].id == id)
				return position;
		return 0;
	}

	// Gives the node `id` its new key. Returns false when the heap does not hold that id: the caller decides what a miss means (the solvers log it once).
	bool rekey(uint32_t id, int32_t newKey)
	{
		const int position = find(id);
		if (position == 0)
		{
			++rekeyMisses_;
			return false;
		}
		changeAt(position, newKey);
		return true;
	}

	// `position` must be one find() returned with no heap change in between (1 .. size()).
	void changeAt(int position, int32_t newKey)
	{
		PQNode& node = nodes_[static_cast<std::size_t>(position)];
		if (newKey > node.key)
		{
			node.key = newKey;
			downHeap(position);
		}
		else if (newKey < node.key)
		{
			node.key = newKey;
			upHeap(position);
		}
	}

	// How many rekey() calls found no node: a solver reports the first one (1) and ignores the rest. Both counters belong to this heap object for its whole
	// life (clear() keeps them), so "once" means once per map object, not once per mission.
	int rekeyMisses() const { return rekeyMisses_; }
	// How many inserts had to reallocate; the first of them logged a line.
	int growths() const { return growths_; }
	int allocations() const { return allocations_; } // reallocations after construction, including reserve()

	const PQNode& at(int position) const { return nodes_[static_cast<std::size_t>(position)]; }

private:
	void upHeap(int index)
	{
		const PQNode start = nodes_[static_cast<std::size_t>(index)];
		while (index > 1 && nodes_[static_cast<std::size_t>(index / 2)].key >= start.key) // bounded at the root: a key below any other cannot run off the top
		{
			nodes_[static_cast<std::size_t>(index)] = nodes_[static_cast<std::size_t>(index / 2)];
			index /= 2;
		}
		nodes_[static_cast<std::size_t>(index)] = start;
	}

	void downHeap(int index)
	{
		const PQNode start = nodes_[static_cast<std::size_t>(index)];
		const int count = size();
		while (index <= count / 2)
		{
			int next = index * 2;
			if (next < count && nodes_[static_cast<std::size_t>(next)].key > nodes_[static_cast<std::size_t>(next + 1)].key)
				++next;
			if (start.key <= nodes_[static_cast<std::size_t>(next)].key)
				break;
			nodes_[static_cast<std::size_t>(index)] = nodes_[static_cast<std::size_t>(next)];
			index = next;
		}
		nodes_[static_cast<std::size_t>(index)] = start;
	}

	int rekeyMisses_ = 0;
	int growths_ = 0;
	int allocations_ = 0;
	std::vector<PQNode> nodes_; // [0] is the filler, [1 .. size()] the heap
};

} // namespace mc2

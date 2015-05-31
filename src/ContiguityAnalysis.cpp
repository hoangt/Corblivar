/*
 * =====================================================================================
 *
 *    Description:  Corblivar contiguity analysis, required for multiple-voltages feature
 *
 *    Copyright (C) 2015 Johann Knechtel, johann.knechtel@ifte.de, www.ifte.de
 *
 *    This file is part of Corblivar.
 *    
 *    Corblivar is free software: you can redistribute it and/or modify it under the terms
 *    of the GNU General Public License as published by the Free Software Foundation,
 *    either version 3 of the License, or (at your option) any later version.
 *    
 *    Corblivar is distributed in the hope that it will be useful, but WITHOUT ANY
 *    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 *    PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *    
 *    You should have received a copy of the GNU General Public License along with
 *    Corblivar.  If not, see <http://www.gnu.org/licenses/>.
 *
 * =====================================================================================
 */

// own Corblivar header
#include "ContiguityAnalysis.hpp"
// required Corblivar headers
#include "Point.hpp"
#include "Block.hpp"
#include "Math.hpp"

void ContiguityAnalysis::analyseBlocks(int layers, std::vector<Block> const& blocks) {
	ContiguityAnalysis::Boundary cur_boundary;
	ContiguityAnalysis::ContiguousNeighbour cur_neighbour;
	std::vector< std::vector<ContiguityAnalysis::Boundary> > boundaries_hor;
	std::vector< std::vector<ContiguityAnalysis::Boundary> > boundaries_vert;
	std::vector<ContiguityAnalysis::Boundary>::iterator i1;
	std::vector<ContiguityAnalysis::Boundary>::iterator i2;

	// for horizontal and vertical contiguity, extract blocks' boundaries, and order
	// them by coordinates; this will reduced required comparisons between pairs of
	// blocks notably by considering only nearby blocks as potential contiguous
	// neighbours
	//

	// init die-wise lists of boundaries
	boundaries_hor.reserve(layers);
	for (int l = 0; l < layers; l++) {
		boundaries_hor.push_back(std::vector<ContiguityAnalysis::Boundary>());
	}
	boundaries_vert.reserve(layers);
	for (int l = 0; l < layers; l++) {
		boundaries_vert.push_back(std::vector<ContiguityAnalysis::Boundary>());
	}

	// add blocks' boundaries into corresponding list
	for (Block const& block : blocks) {

		// reset previous contiguous neighbours
		block.contiguous_neighbours.clear();

		cur_boundary.block = &block;

		// left boundary
		cur_boundary.p1.x = block.bb.ll.x;
		cur_boundary.p1.y = block.bb.ll.y;
		cur_boundary.p2.x = block.bb.ll.x;
		cur_boundary.p2.y = block.bb.ur.y;

		boundaries_vert[block.layer].push_back(cur_boundary);

		// right boundary
		cur_boundary.p1.x = block.bb.ur.x;
		cur_boundary.p1.y = block.bb.ll.y;
		cur_boundary.p2.x = block.bb.ur.x;
		cur_boundary.p2.y = block.bb.ur.y;

		boundaries_vert[block.layer].push_back(cur_boundary);

		// bottom boundary
		cur_boundary.p1.x = block.bb.ll.x;
		cur_boundary.p1.y = block.bb.ll.y;
		cur_boundary.p2.x = block.bb.ur.x;
		cur_boundary.p2.y = block.bb.ll.y;

		boundaries_hor[block.layer].push_back(cur_boundary);

		// top boundary
		cur_boundary.p1.x = block.bb.ll.x;
		cur_boundary.p1.y = block.bb.ur.y;
		cur_boundary.p2.x = block.bb.ur.x;
		cur_boundary.p2.y = block.bb.ur.y;

		boundaries_hor[block.layer].push_back(cur_boundary);

		// TODO drop dummy data
		cur_neighbour.neighbour = &block;
		block.contiguous_neighbours.push_back(cur_neighbour);
	}

	// determine horizontal and vertical contiguous neighbours die-wise
	//
	for (int l = 0; l < layers; l++) {

		// first, sort boundaries such that they are ordered by their orthogonal
		// dimension first (i.e., y for horizontal, and x for vertical boundaries)
		// and also by their extension dimension; this way, boundaries can next be
		// easily compared with each other
		std::sort(boundaries_hor[l].begin(), boundaries_hor[l].end(), ContiguityAnalysis::boundaries_hor_comp);
		std::sort(boundaries_vert[l].begin(), boundaries_vert[l].end(), ContiguityAnalysis::boundaries_vert_comp);

		if (ContiguityAnalysis::DBG) {

			std::cout << "DBG_CONTIGUITY> Sorted boundaries; die " << l << "; horizontal boundaries:" << std::endl;
			for (auto const& boundary : boundaries_hor[l]) {

				std::cout << "DBG_CONTIGUITY>  Boundary: ";
				std::cout << "(" << boundary.p1.x << "," << boundary.p1.y << ")";
				std::cout << "(" << boundary.p2.x << "," << boundary.p2.y << "); block " << boundary.block->id << std::endl;
			}

			std::cout << "DBG_CONTIGUITY> Sorted boundaries; die " << l << "; vertical boundaries:" << std::endl;
			for (auto const& boundary : boundaries_vert[l]) {

				std::cout << "DBG_CONTIGUITY>  Boundary: ";
				std::cout << "(" << boundary.p1.x << "," << boundary.p1.y << ")";
				std::cout << "(" << boundary.p2.x << "," << boundary.p2.y << "); block " << boundary.block->id << std::endl;
			}
		}

		// then, walk boundaries and whenever two boundaries are intersecting on
		// the same x- and y-coordinates, consider their related blocks as
		// contiguous neighbours
		//
		if (ContiguityAnalysis::DBG) {

			std::cout << "DBG_CONTIGUITY> Determine intersecting boundaries; derive contiguity" << std::endl;
		}

		// vertical boundaries
		for (i1 = boundaries_vert[l].begin(); i1 != boundaries_vert[l].end(); ++i1) {

			ContiguityAnalysis::Boundary& b1 = (*i1);

			if (ContiguityAnalysis::DBG) {
				std::cout << "DBG_CONTIGUITY>  Currently considered vertical segment ";
				std::cout << "(" << b1.p1.x << "," << b1.p1.y << ")";
				std::cout << "(" << b1.p2.x << "," << b1.p2.y << "); block " << b1.block->id << std::endl;
			}

			// the boundary b2, to be compared to b1, should have the same
			// x-coordinate; thus, we start from the next element (not the
			// same, in order to avoid comparison with itself) in the set of
			// sorted boundaries
			i2 = i1 + 1;
			for (; i2 != boundaries_vert[l].end(); ++i2) {

				ContiguityAnalysis::Boundary& b2 = (*i2);

				// break loop conditions;
				if (
					// if the lower point of b2 is just matching or
					// already above the upper point of b1, i.e., when
					// no intersection is feasible anymore
					(Math::doubleComp(b2.p1.y, b1.p2.y) || b2.p1.y > b1.p2.y)
					// or if we reached the next x-coordinate already,
					// i.e., again no intersection is feasible anymore
					|| (b2.p1.x > b1.p1.x)
				   ) {
					break;
				}

				// TODO
				// otherwise, check for and determine amount of
				// intersection; memorize intersection as common boundary

				// TODO only report actually intersecting boundaries,
				// i.e., contiguity of blocks
				if (ContiguityAnalysis::DBG) {
					std::cout << "DBG_CONTIGUITY>   Horizontal segment to consider as potentially intersecting";
					std::cout << "(" << b2.p1.x << "," << b2.p1.y << ")";
					std::cout << "(" << b2.p2.x << "," << b2.p2.y << "); block " << b2.block->id << std::endl;
				}
			}

		}
	}

	if (ContiguityAnalysis::DBG) {
		std::cout << "DBG_CONTIGUITY> Contiguous neighbours for all blocks:" << std::endl;

		for (Block const& block : blocks) {

			std::cout << "DBG_CONTIGUITY>  Block " << block.id << ":";

			for (auto& cont_neighbour : block.contiguous_neighbours) {
				std::cout << " " << cont_neighbour.neighbour->id;
			}
			std::cout << std::endl;
		}
	}
}

inline bool ContiguityAnalysis::boundaries_vert_comp(ContiguityAnalysis::Boundary const& b1, ContiguityAnalysis::Boundary const& b2) {
	return (
			// x-coordinates are the first criterion; note
			// that it's sufficient to compare the first
			// (lower) points since it's a vertical segment
			(b1.p1.x < b2.p1.x)
			// for boundaries with same x-coordinate, resolve
			// equal values by considering the boundaries'
			// y-coordinate
			|| (Math::doubleComp(b1.p1.x, b2.p1.x) && (b1.p1.y < b2.p1.y))
			// there will also be cases when segments start on
			// the same x- and y-coordinate; then, resolve by
			// block order, i.e., the segment of the left
			// block comes first
			|| (Math::doubleComp(b1.p1.x, b2.p1.x) && Math::doubleComp(b1.p1.y, b2.p1.y) && (b1.block->bb.ll.x < b2.block->bb.ll.x))
	       );
};

inline bool ContiguityAnalysis::boundaries_hor_comp(ContiguityAnalysis::Boundary const& b1, ContiguityAnalysis::Boundary const& b2) {
	return (
			// y-coordinates are the first criterion; note
			// that it's sufficient to compare the first
			// (left) points since it's a horizontal segment
			(b1.p1.y < b2.p1.y)
			// for boundaries with same y-coordinate, resolve
			// equal values by considering the boundaries'
			// x-coordinate
			|| (Math::doubleComp(b1.p1.y, b2.p1.y) && (b1.p1.x < b2.p1.x))
			// there will also be cases when segments start on
			// the same y- and x-coordinate; then, resolve by
			// block order, i.e., the segment of the lower
			// block comes first
			|| (Math::doubleComp(b1.p1.y, b2.p1.y) && Math::doubleComp(b1.p1.x, b2.p1.x) && (b1.block->bb.ll.y < b2.block->bb.ll.y))
	       );
};

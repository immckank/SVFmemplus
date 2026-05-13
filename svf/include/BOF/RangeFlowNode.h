//===- RangeFlowNode.h -- Record Value Flow from Parent to Children -------------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

/*
 * RangeFlowNode.h
 *
 *  Created on: NOV 10, 2025
 *      Author: Yaokun Yang
 */

// RangeFlowNode.h
#ifndef RANGE_FLOW_NODE_H_
#define RANGE_FLOW_NODE_H_

#include "MemoryModel/PointerAnalysisImpl.h"
#include "Range.h"


namespace SVF {
    class RangeFlowNode{
        public:
            const SVFVar* base;
            const SVFVar* parent;
            Range accumulate_offset;
            const bool isHeap;

            RangeFlowNode(const SVFVar* _base, const SVFVar* _parent, Range _accumulate_offset, bool _isHeap = false);
    };
}

#endif

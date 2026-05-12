//===----- RangeFlowNode.cpp -- Record Value Flow from Parent to Children --------------//
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
 *  RangeFlowNode.cpp 
 *
 *  Created on: Mar 15 , 2025
 *      Author: Yaokun Yang
 */


#include "BOF/RangeFlowNode.h"
using namespace SVF;
using namespace std;

RangeFlowNode::RangeFlowNode(const SVFVar* _base, const SVFVar* _parent, Range _accumulate_offset, bool _isHeap)
    : base(_base), parent(_parent), accumulate_offset(_accumulate_offset), isHeap(_isHeap){}
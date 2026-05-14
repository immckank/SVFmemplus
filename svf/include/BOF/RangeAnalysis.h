//===- RangeAnalysis.h -- Range Analysis of SVF Vars -------------------------------//
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
 * RangeAnalysis.h
 *
 *  Created on: MAR 15, 2025
 *      Author: Yaokun Yang
 */

 // RangeAnalysis.h
#ifndef RANGE_ANALYSIS_H
#define RANGE_ANALYSIS_H

#include "MemoryModel/PointerAnalysisImpl.h"
#include <unordered_map>
#include "Range.h"


namespace SVF {
    class RangeAnalysis {
        public:
            RangeAnalysis();
            bool analyzeBufferRange(const StackObjVar* stackObjVar);
            bool analyzeBufferRange(const HeapObjVar* heapObjVar);
            bool setBufferByteRange(const SVFVar* buffer, const Range& byteSizeRange);
            Range analyzeVarRange(const SVFVar* var, int depth = 0);
            Range getBufferRange(const SVFVar* buffer);
            Range getVarRange(const SVFVar* var);

        private:
            const static int MAX_RECURSION_DEPTH;
            std::unordered_map<const SVFVar*, Range> bufferRanges;
            std::unordered_map<const SVFVar*, Range> varRanges;
    };
}

#endif /* RANGE_ANALYSIS_H_ */

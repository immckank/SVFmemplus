//===- BOFChecker.h -- Detecting Buffer Overflow Errors -------------------------------//
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
 * BufferOverflowChecker.h
 *
 *  Created on: NOV 10, 2025
 *      Author: Yaokun Yang
 */

// BufferOverflowChecker.h
#ifndef BUFFER_OVERFLOW_CHECKER_H_
#define BUFFER_OVERFLOW_CHECKER_H_

#include "MemoryModel/PointerAnalysisImpl.h"
#include "RangeFlowNode.h"
#include "RangeAnalysis.h"

#include <queue>


namespace SVF {
    class BufferOverflowChecker {
        public:
            void runOnModule(SVFIR* pag);
            void initialize(SVFIR* pag);
            void propagate(SVFIR* pag);
            void reportBufferOverflowError(const SVFVar* base, const SVFType* type, const Range& offset, const Range& size);

        private:
            std::queue<RangeFlowNode>worklist;
            RangeAnalysis rangeAnalysis;
    };
}

#endif /* BUFFER_OVERFLOW_CHECKER_H_ */
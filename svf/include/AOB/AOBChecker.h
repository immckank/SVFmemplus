//===- AOBChecker.h -- Detecting Array Out-of-Bounds-------------------------------//
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
 * AOBChecker.h
 *
 *  Created on: NOV 10, 2025
 *      Author: Yaokun Yang
 */

// AOBChecker.h
#ifndef AOBCHECKER_H_
#define AOBCHECKER_H_

#include "MemoryModel/PointerAnalysisImpl.h"
#include <unordered_map>
#include <queue>
#include <iostream>

namespace SVF {

class AOBchecker {
public:
    struct BFSNode{
        const SVFVar* node;
        s64_t offset;
        s64_t size;

        BFSNode(const SVFVar* n = nullptr, s64_t off = 0, s64_t s = 0)
        : node(n), offset(off), size(s) {}
    };

    void runOnModule(SVFIR* pag);
    void propagate(SVFIR* pag);
    void report(const SVFVar* base, s64_t fldIdx, s64_t arraySize);

private:
    std::queue<BFSNode> worklist;
};

}

#endif /* AOBCHECKER_H_ */
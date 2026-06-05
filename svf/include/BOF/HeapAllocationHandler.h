//===- HeapAllocationHandler.h -- Handle heap allocation -------------------------------//
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
 * heapAllocationHandler.h
 *
 *  Created on: May 11, 2026
 *      Author: Yaokun Yang
 */

#ifndef HEAP_ALLOCATION_HANDLER_H
#define HEAP_ALLOCATION_HANDLER_H

#include "RangeAnalysis.h"
#include "AllocAPIRegistry.h"

namespace SVF {

class CallICFGNode;

/// Symbolic description of an allocation's size, in terms of the *actual*
/// SVFVar operand(s) at the call site (rather than a folded numeric range).
/// Used to relate an allocation's size to a later copy length even when the
/// concrete value is unknown (TOP). For a malloc-style call the size is
/// `factor (==1) * <sizeVar>`; for a calloc-style call it is
/// `elemFactor * <sizeVar(count)>` and @ref isElemMul is set.
struct AllocSizeSym {
    const SVFVar* sizeVar = nullptr;  ///< the symbolic size / element-count operand
    long long factor = 1;             ///< constant byte multiplier (calloc element size)
    bool isElemMul = false;           ///< true for calloc-style element*count
};

class HeapAllocationHandler {
public:
    HeapAllocationHandler(RangeAnalysis* _ra);

    /**
     * @brief Checks whether the called function of @p call is a recognized
     *        heap allocation API.
     *
     * Identification is delegated to AllocAPIRegistry, which reuses
     * SaberCheckerAPI / ExtAPI and a BOF-local supplementary table.
     *
     * @param fun The callee FunObjVar to check.
     * @return true if @p fun is a recognized allocation API.
     */
    bool isAllocAPI(const FunObjVar* fun) const;

    /**
     * @brief Computes the allocation size (in bytes / elements as encoded by
     *        the source) for an allocation call, reading the *actual* arguments
     *        at the call site according to the data-driven AllocSpec.
     *
     * @param call The allocation call-site ICFG node.
     *
     * @return A Range representing the computed allocation size; Range(0) if the
     *         callee is not a recognized allocation API or the size cannot be
     *         determined.
     */
    Range analyzeAllocSize(const CallICFGNode* call);

    /**
     * @brief Resolves the *symbolic* size operand(s) of an allocation call,
     *        for relating allocation size to copy length under unknown ranges.
     *
     * @param call The allocation call-site ICFG node.
     * @param out  Filled with the size operand / factor on success.
     * @return true if @p call is a recognized allocation API with a resolvable
     *         size operand.
     */
    bool getAllocSizeOperand(const CallICFGNode* call, AllocSizeSym& out);

private:
    enum AllocSizeKind {
        AS_UNKNOWN,
        AS_ARG,
        AS_MUL_ARGS
    };

    struct AllocSizeSpec {
        u32_t requiredArgs;
        AllocSizeKind kind;
        u32_t arg0;
        u32_t arg1;
    };

    RangeAnalysis* ra;
    AllocAPIRegistry registry;
};
}

#endif

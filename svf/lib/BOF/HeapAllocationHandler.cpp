//===- HeapAllocationHandler.cpp -- Handle heap allocation --------------------//
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
 * HeapAllocationHandler.cpp
 *
 *  Created on: May 11, 2026
 *      Author: Yaokun Yang
 */

#include "BOF/HeapAllocationHandler.h"
#include "Graphs/ICFGNode.h"

using namespace SVF;

HeapAllocationHandler::HeapAllocationHandler(RangeAnalysis* _ra) : ra(_ra) {}

bool HeapAllocationHandler::isAllocAPI(const FunObjVar* fun) const {
    return registry.isAllocAPI(fun);
}

Range HeapAllocationHandler::analyzeAllocSize(const CallICFGNode* call) {
    const FunObjVar* fun = call->getCalledFunction();
    AllocSpec spec;
    if (!fun || !registry.resolveAlloc(fun, spec))
        return Range(0);

    const u32_t argn = call->arg_size();

    // calloc-style: size = arg[count] * arg[size]
    if (spec.kind == AllocSpec::ELEM_MUL_ARG) {
        if (spec.countArgIdx < 0 || spec.sizeArgIdx < 0 ||
            (u32_t)spec.countArgIdx >= argn || (u32_t)spec.sizeArgIdx >= argn)
            return Range(0);
        Range countRange = ra->analyzeVarRange(call->getArgument(spec.countArgIdx));
        Range sizeRange  = ra->analyzeVarRange(call->getArgument(spec.sizeArgIdx));
        return Range::mul(countRange, sizeRange);
    }

    // malloc / realloc style: size = arg[sizeArgIdx] (read from actual call args)
    if (spec.sizeArgIdx < 0 || (u32_t)spec.sizeArgIdx >= argn)
        return Range(0);
    return ra->analyzeVarRange(call->getArgument(spec.sizeArgIdx));
}

bool HeapAllocationHandler::getAllocSizeOperand(const CallICFGNode* call,
                                                AllocSizeSym& out) {
    const FunObjVar* fun = call->getCalledFunction();
    AllocSpec spec;
    if (!fun || !registry.resolveAlloc(fun, spec))
        return false;

    const u32_t argn = call->arg_size();

    if (spec.kind == AllocSpec::ELEM_MUL_ARG) {
        if (spec.countArgIdx < 0 || spec.sizeArgIdx < 0 ||
            (u32_t)spec.countArgIdx >= argn || (u32_t)spec.sizeArgIdx >= argn)
            return false;
        out.isElemMul = true;
        out.sizeVar = call->getArgument(spec.countArgIdx);
        // Element byte size, if a positive constant.
        Range elem = ra->analyzeVarRange(call->getArgument(spec.sizeArgIdx));
        out.factor = (elem.isConstant() && elem.getLower() > 0) ? elem.getLower() : 0;
        return out.sizeVar != nullptr;
    }

    if (spec.sizeArgIdx < 0 || (u32_t)spec.sizeArgIdx >= argn)
        return false;
    out.isElemMul = false;
    out.factor = 1;
    out.sizeVar = call->getArgument(spec.sizeArgIdx);
    return out.sizeVar != nullptr;
}

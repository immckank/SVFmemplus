//===- AllocAPIRegistry.h -- Unified heap allocation API registry -------------//
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
 * AllocAPIRegistry.h
 *
 *  Created on: JUN 04, 2026
 *      Author: Yaokun Yang
 */

#ifndef ALLOC_API_REGISTRY_H
#define ALLOC_API_REGISTRY_H

#include "SVFIR/SVFVariables.h"
#include <map>
#include <string>

namespace SVF
{

/**
 * @brief Data-driven specification describing how to compute the allocation
 *        size of a recognized allocation API.
 *
 * - SIZE_ARG      : size = arg[sizeArgIdx]                 (malloc / realloc ...)
 * - ELEM_MUL_ARG  : size = arg[countArgIdx] * arg[sizeArgIdx]  (calloc ...)
 */
struct AllocSpec
{
    enum Kind { SIZE_ARG, ELEM_MUL_ARG };
    Kind kind = SIZE_ARG;
    int  sizeArgIdx  = 0;   ///< malloc=0, realloc=1, LOS_MemRealloc=2 ...
    int  countArgIdx = -1;  ///< calloc element-count arg (only for ELEM_MUL_ARG)
};

/**
 * @brief Unified registry that decides whether a call target is a heap
 *        allocation function and how to derive its allocation size.
 *
 * Identification merges several sources (the BOF module never modifies SVF
 * master data such as extapi.json; it only reuses interfaces read-only):
 *   1. BOF local table  — most precise size spec (calloc/realloc/platform funcs)
 *   2. SaberCheckerAPI::isMemAlloc(fun) — reuse SABER's CK_ALLOC big table
 *   3. ExtAPI annotations (ALLOC_HEAP_RET / is_alloc / is_realloc)
 *
 * The HeapObjVar-modeled path (priority ①) is handled directly in the checker's
 * Addr branch via getByteSizeOfObj(); this registry covers the Call branch.
 */
class AllocAPIRegistry
{
public:
    AllocAPIRegistry();

    /// @return true if @p fun is a recognized allocation API, filling @p out.
    bool resolveAlloc(const FunObjVar* fun, AllocSpec& out) const;

    /// @return true if @p fun is a recognized allocation API.
    bool isAllocAPI(const FunObjVar* fun) const;

private:
    /// BOF-local supplementary table: function name -> precise allocation spec.
    std::map<std::string, AllocSpec> localTable;
};

} // namespace SVF

#endif /* ALLOC_API_REGISTRY_H */

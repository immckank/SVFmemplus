//===- AllocAPIRegistry.cpp -- Unified heap allocation API registry -----------//
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
 * AllocAPIRegistry.cpp
 *
 *  Created on: JUN 04, 2026
 *      Author: Yaokun Yang
 */

#include "BOF/AllocAPIRegistry.h"
#include "SABER/SaberCheckerAPI.h"
#include "Util/ExtAPI.h"

using namespace SVF;

AllocAPIRegistry::AllocAPIRegistry()
{
    // ===== BOF local supplementary table =====
    // malloc-style: size = arg0
    localTable["malloc"]          = {AllocSpec::SIZE_ARG, 0, -1};
    localTable["xmalloc"]         = {AllocSpec::SIZE_ARG, 0, -1};
    localTable["valloc"]          = {AllocSpec::SIZE_ARG, 0, -1};
    localTable["alloca"]          = {AllocSpec::SIZE_ARG, 0, -1};
    localTable["_TIFFmalloc"]     = {AllocSpec::SIZE_ARG, 0, -1};
    localTable["VOS_MemAlloc"]    = {AllocSpec::SIZE_ARG, 0, -1};
    localTable["strdup"]          = {AllocSpec::SIZE_ARG, 0, -1};
    localTable["kmem_cache_alloc"]= {AllocSpec::SIZE_ARG, 0, -1};

    // strndup(s, n) -> n bytes
    localTable["strndup"]         = {AllocSpec::SIZE_ARG, 1, -1};

    // realloc(ptr, size) -> size at arg1 (fixes the old "always arg0" bug)
    localTable["realloc"]         = {AllocSpec::SIZE_ARG, 1, -1};
    // LOS_MemAlloc(pool, size) -> size at arg1
    localTable["LOS_MemAlloc"]    = {AllocSpec::SIZE_ARG, 1, -1};
    // LOS_MemRealloc(pool, ptr, size) -> size at arg2
    localTable["LOS_MemRealloc"]  = {AllocSpec::SIZE_ARG, 2, -1};

    // OpenHarmony OSAL allocators: single size argument (arg0).
    //   void *OsalMemAlloc(size_t size);
    //   void *OsalMemCalloc(size_t size);   // zero-initialised, still 1 arg
    //   void *OsalMemAllocAlign(size_t alignment, size_t size); // size at arg1
    localTable["OsalMemAlloc"]    = {AllocSpec::SIZE_ARG, 0, -1};
    localTable["OsalMemCalloc"]   = {AllocSpec::SIZE_ARG, 0, -1};
    localTable["OsalMemAllocAlign"] = {AllocSpec::SIZE_ARG, 1, -1};

    // calloc-style: size = arg[count] * arg[size]
    localTable["calloc"]          = {AllocSpec::ELEM_MUL_ARG, 1, 0};
    localTable["SoftBusCalloc"]   = {AllocSpec::ELEM_MUL_ARG, 1, 0};
    localTable["SysCalloc"]       = {AllocSpec::ELEM_MUL_ARG, 1, 0};
}

bool AllocAPIRegistry::resolveAlloc(const FunObjVar* fun, AllocSpec& out) const
{
    if (!fun)
        return false;

    // ① BOF local table provides the most precise size spec (covers calloc /
    //    realloc / platform-private allocators with correct argument indices).
    auto it = localTable.find(fun->getName());
    if (it != localTable.end())
    {
        out = it->second;
        return true;
    }

    // ②③ Reuse SVF's existing knowledge for identification (read-only):
    //     SaberCheckerAPI's CK_ALLOC table and ExtAPI annotations.
    ExtAPI* ext = ExtAPI::getExtAPI();
    bool identified =
        SaberCheckerAPI::getCheckerAPI()->isMemAlloc(fun) ||
        ext->is_alloc(fun) ||
        ext->is_realloc(fun) ||
        ext->hasExtFuncAnnotation(fun, "ALLOC_HEAP_RET");

    if (identified)
    {
        // realloc(ptr, size): size is at arg1; other malloc-style: arg0.
        out.kind = AllocSpec::SIZE_ARG;
        out.sizeArgIdx = ext->is_realloc(fun) ? 1 : 0;
        out.countArgIdx = -1;
        return true;
    }

    return false;
}

bool AllocAPIRegistry::isAllocAPI(const FunObjVar* fun) const
{
    AllocSpec spec;
    return resolveAlloc(fun, spec);
}

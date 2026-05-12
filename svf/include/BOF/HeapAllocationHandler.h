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

namespace SVF {

class HeapAllocationHandler {
public:
    HeapAllocationHandler(RangeAnalysis* _ra);

    /**
     * @brief Checks whether the function name corresponds to a known allocation API,
     *        and validates the number of arguments passed to it.
     *
     * @param funcName          The name of the function to check (e.g., "malloc", "new", "calloc").
     * @param actualParamCount  The actual number of arguments passed to the function in the source code.
     *
     * @return true  If `funcName` is a known allocation API and `actualParamCount` matches
     *               the expected parameter count for that API.
     * @return false Otherwise (unknown API or mismatched parameter count).
     */
    bool isAllocAPI(const std::string& funcName, size_t actualParamCount) const;


    /**
     * @brief Parses a CallPE statement and computes the allocation size.
     *
     * This function analyzes the given call expression, checks if it corresponds
     * to a known allocation API (e.g., malloc, calloc, new, etc.), and attempts
     * to evaluate the total number of bytes being allocated.
     *
     * @param funObjVar A pointer to the FunObjVar representing the function object
     *                  of the call to analyze.
     *
     * @return A Range object representing the computed allocation size.
     *         - If the function is a recognized allocation API, returns the Range
     *           of the total allocation size (in bytes).
     *         - If the function is NOT a recognized allocation API, returns an
     *           invalid Range or a Range representing zero (0).
     *
     * @note The returned Range may be symbolic (e.g., representing an unknown value)
     *       if the allocation size cannot be determined at compile time.
     */
    Range analyzeAllocSize(const FunObjVar* funObjVar);

private:
    RangeAnalysis* ra;

    /**
     * @brief Map of allocation API function names to their expected parameter counts.
     * 
     * This map defines the recognized heap allocation functions and the number of
     * parameters each function expects. It is independent of SABER's internal
     * implementation and serves as the configuration for identifying allocation
     * APIs during analysis.
     * 
     * @note Storage structure: function name -> expected parameter count
     */
    static const std::map<std::string, u32_t> allocApiMap;
};
}

#endif
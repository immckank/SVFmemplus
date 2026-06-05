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
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "Range.h"


namespace SVF {
    class ICFGNode;

    /// A symbolic affine value of the form `base_symbol + offset`.
    ///
    /// `base` is a *structural* identity string of the underlying symbol
    /// (canonicalised so that several SSA values denoting the same source
    /// expression — e.g. two separate loads of the same struct field under
    /// -O0 — share the same `base`). An empty `base` denotes a pure integer
    /// constant whose value is `offset`.
    struct AffineTerm {
        std::string base;   ///< structural symbol identity ("" => constant)
        long long offset;   ///< constant offset added to the symbol
    };

    class RangeAnalysis {
        public:
            RangeAnalysis();
            bool analyzeBufferRange(const StackObjVar* stackObjVar);
            bool analyzeBufferRange(const HeapObjVar* heapObjVar);
            Range analyzeVarRange(const SVFVar* var, int depth = 0);

            /// Compute the set of symbolic affine forms a scalar integer
            /// variable may take. Branch merges (phi/select) and conditional
            /// memory accumulators (store/load through a local slot) yield
            /// several terms. Loads of read-only memory (e.g. a deserialised
            /// struct field) bottom out as an opaque symbol identified by the
            /// structural location token of the address being read, so that
            /// two distinct loads of the *same* location compare equal.
            ///
            /// Used by the buffer-overflow checker to prove "allocated size <
            /// copied length" relations even when the concrete numeric range is
            /// unknown (TOP).
            std::vector<AffineTerm> analyzeAffine(const SVFVar* var);

            /// Structural identity token of a value / address SVFVar. Two
            /// variables that denote the same source-level memory location or
            /// expression (across separate SSA loads / geps) produce identical
            /// tokens, enabling cross-instruction matching without points-to.
            std::string locationToken(const SVFVar* var);

            /// Context-sensitive (k=1 call-site) variant: when @p context is
            /// non-null, formal parameters bound at that call site (see
            /// bindFormalRange) resolve to their actual-argument range instead
            /// of degrading to TOP across the Call edge.
            Range analyzeVarRange(const SVFVar* var, const ICFGNode* context, int depth = 0);

            /// Bind a callee formal parameter's range at a specific call site so
            /// that context-sensitive analyzeVarRange can recover it.
            void bindFormalRange(const ICFGNode* context, const SVFVar* formal,
                                 const Range& range);

            Range getBufferRange(const SVFVar* buffer);
            Range getVarRange(const SVFVar* var);

            /// Register a buffer's valid index/byte range (used for allocation
            /// call sites whose size is computed from call arguments rather than
            /// from a HeapObjVar/StackObjVar model).
            void setBufferRange(const SVFVar* buffer, const Range& range);

        private:
            const static int MAX_RECURSION_DEPTH;
            const static int MAX_AFFINE_DEPTH;
            const static size_t MAX_AFFINE_TERMS;

            /// Recursive worker for analyzeAffine (cycle-broken via @p visited).
            std::vector<AffineTerm> analyzeAffine(const SVFVar* var,
                                                  std::set<const SVFVar*>& visited,
                                                  int depth);
            /// Collect the affine forms of every value stored into @p addr.
            std::vector<AffineTerm> affineStoredInto(const SVFVar* addr,
                                                     std::set<const SVFVar*>& visited,
                                                     int depth);
            /// Recursive worker for locationToken.
            std::string structToken(const SVFVar* var, int depth,
                                     std::set<const SVFVar*>& visited);

            /// Cache get/set keyed by (context, var); context==null falls back to
            /// the context-insensitive varRanges cache.
            Range getCachedVarRange(const ICFGNode* context, const SVFVar* var);
            void setCachedVarRange(const ICFGNode* context, const SVFVar* var,
                                   const Range& range);

            std::unordered_map<const SVFVar*, Range> bufferRanges;
            std::unordered_map<const SVFVar*, Range> varRanges;
            /// (call-site context, formal param) -> bound actual-argument range.
            std::map<std::pair<const ICFGNode*, const SVFVar*>, Range> formalBindings;
            /// (call-site context, var) -> context-sensitive range cache.
            std::map<std::pair<const ICFGNode*, const SVFVar*>, Range> ctxVarRanges;
    };
}

#endif /* RANGE_ANALYSIS_H_ */

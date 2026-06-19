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


#ifndef BUFFER_OVERFLOW_CHECKER_H_
#define BUFFER_OVERFLOW_CHECKER_H_

#include "MemoryModel/PointerAnalysisImpl.h"
#include "RangeFlowNode.h"
#include "RangeAnalysis.h"
#include "HeapAllocationHandler.h"
#include "MemCopyAPIRegistry.h"
#include "LLMTriage.h"
#include "Util/SVFBugReport.h"

#include <queue>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>


namespace SVF {

    /// Source of a detected buffer-overflow (for structured reporting).
    enum class BofKind { GEP_OOB, MEMCPY_OOB, MEMSET_OOB, STRCPY_OOB };

    class BufferOverflowChecker {
        public:
            BufferOverflowChecker();
            void runOnModule(SVFIR* pag);
            void initialize(SVFIR* pag);
            void propagate(SVFIR* pag);

            /// Memory-copy / string API write-and-read overflow checks (feature 2).
            void checkMemoryOps(SVFIR* pag);

            /// Context-sensitive (k=1) pre-pass: bind each callee scalar-integer
            /// formal parameter to its actual-argument range at every direct call
            /// site, so that interprocedurally-forwarded constant indices (e.g.
            /// write_at(p, 11)) resolve precisely instead of degrading to TOP.
            void bindCallArguments(SVFIR* pag);

            /**
             * @brief Records a candidate buffer overflow (deduplicated by source
             *        location, kind and call context) into @ref pendingReports.
             *        Actual emission to SVFBugReport / terminal is deferred to
             *        flushReports(), which performs cross-context dedup (a MUST at
             *        an access point suppresses MAYs at the same point).
             */
            void reportBufferOverflowError(const SVFVar* base, const Range& offset,
                                           const Range& size, bool isHeap,
                                           BofKind kind, const ICFGNode* loc,
                                           bool mustOverflow,
                                           const ICFGNode* callContext = nullptr,
                                           const SVFVar* indexVar = nullptr);

            /// Flush deferred reports: at every access point (source location +
            /// kind) where some calling context proves a MUST overflow, suppress
            /// the lower-information MAY reports (e.g. the null-context MAY) so the
            /// same point is not double-reported. Emits the surviving reports to
            /// SVFBugReport and the terminal.
            void flushReports();

            /// Dump structured bug report to a JSON file.
            void dumpReport(const std::string& filePath);

            /// Configure the (optional) LLM MAY-triage overlay. Must be called
            /// before runOnModule(). When unset, triage stays in "API-empty"
            /// mode: slices are still exported for manual review.
            void setLLMTriageConfig(const LLMTriageConfig& cfg)
            {
                llmTriage.setConfig(cfg);
            }

        private:
            /// Per-edge dispatch handlers (split from the old monolithic propagate).
            void handleGep(const GepStmt* gepStmt, const RangeFlowNode& srcNode);
            void handleCopyLike(const SVFVar* dstVar, const RangeFlowNode& srcNode,
                                const ICFGNode* loc);
            void handleCall(const CallPE* callPE, const RangeFlowNode& srcNode);
            void handleRet(const RetPE* retPE, const RangeFlowNode& srcNode);

            /// Fixpoint enqueue: "enqueue only if state changed" + widening, keyed
            /// on (buffer-root, current-var) to guarantee termination over loops /
            /// recursive / interprocedural back-edges.
            void enqueue(const SVFVar* base, const SVFVar* parent, const Range& offset,
                         bool isHeap, uint8_t callDepth,
                         const ICFGNode* callContext = nullptr);

            /// Context-sensitive seeding helper (Fix B): in addition to the
            /// context-insensitive (null) seed, also seed a buffer allocated
            /// inside function @p enclosingFun under each of @p enclosingFun's
            /// direct call sites (bounded by MAX_SEED_CONTEXTS). The k=1 formal
            /// bindings established by bindCallArguments then let an index that
            /// derives from a callee formal (e.g. `lender_cnt`) resolve to its
            /// precise actual range, upgrading an otherwise MAY overflow to MUST.
            void seedBufferUnderCallContexts(SVFIR* pag, const SVFVar* base,
                                             const SVFVar* parent, bool isHeap,
                                             const FunObjVar* enclosingFun);

            /// Classify and check an access offset against a buffer size.
            /// @p callContext (k=1 call-site, may be null) distinguishes the same
            /// callee instruction analyzed under different calling contexts so that
            /// per-context must-overflows are each reported (not collapsed by the
            /// source-location dedup).
            void checkAccess(const SVFVar* dstVar, const Range& offset, const Range& size,
                             bool isHeap, BofKind kind, const ICFGNode* loc,
                             const ICFGNode* callContext = nullptr,
                             const SVFVar* indexVar = nullptr);

            /// Recover the byte-domain capacity and byte offset of a buffer
            /// pointer @p var (used by checkMemoryOps). Heap buffers are already
            /// byte-modeled; stack buffers convert element offsets to bytes using
            /// the object's element byte size.
            /// @return false if @p var is not tracked as a buffer.
            bool getByteBuffer(const SVFVar* var, signed long long& capacity,
                               Range& byteOffset, bool& isHeap);

            /// Symbolic under-allocation remediation (BO-4 / BO-5). When the
            /// numeric copy length is unknown (TOP) or the destination buffer is
            /// loaded from a struct field the worklist does not connect, try to
            /// prove that the allocation feeding @p bufArg is *symbolically*
            /// smaller than the copied length @p lenVar (e.g. `malloc(len-1)`
            /// then copy `len` bytes). Both sizes are expressed as affine forms
            /// over a shared symbol; a report is emitted only when a
            /// strictly-smaller relation is proven (zero new false positives),
            /// classified MUST if every alloc branch overflows else MAY.
            /// @p byteOffUpper is the highest write start offset within the
            /// buffer (0 for whole-buffer writes).
            void trySymbolicUnderAlloc(const SVFVar* bufArg, const SVFVar* lenVar,
                                       signed long long byteOffUpper,
                                       const ICFGNode* loc);

            /// Resolve the symbolic allocation size feeding the buffer pointer
            /// @p bufArg, either via its tracked allocation-result var, or by
            /// matching the structural location it was loaded from against a
            /// recorded allocation-result store. @return false if not resolvable.
            bool findAllocSizeForBuffer(const SVFVar* bufArg, AllocSizeSym& out);

            /// Record an allocation's symbolic size, keyed by both the result var
            /// and the structural token of any field/slot it is stored into.
            void recordAllocSizeSym(const SVFVar* resultVar, const AllocSizeSym& aSym);

            /// Interprocedural (plan A) expansion depth bound, to prevent divergence.
            static const uint8_t MAX_CALL_DEPTH;

            /// Upper bound on the number of distinct call-site contexts a single
            /// buffer is seeded under (Fix B). Caps the context blow-up so the
            /// fixpoint state stays finite / fast; buffers in heavily-called
            /// functions fall back to the null-context seed (still MAY-sound).
            static const uint8_t MAX_SEED_CONTEXTS;

            /// Aggregated buffer membership of a variable, used by checkMemoryOps
            /// to recover the buffer root / accumulated offset of a pointer
            /// argument passed to a memory-copy / string API.
            struct BufferInfo {
                const SVFVar* parent = nullptr;  ///< buffer root object
                Range offset;                    ///< accumulated offset from root
                bool isHeap = false;             ///< byte-domain (heap) buffer?
            };

            /// A buffer-overflow candidate captured during analysis, emitted only
            /// after flushReports() resolves cross-context MUST/MAY priority.
            struct PendingReport {
                const SVFVar* base = nullptr;
                Range offset;
                Range size;
                bool isHeap = false;
                BofKind kind = BofKind::GEP_OOB;
                const ICFGNode* loc = nullptr;
                bool mustOverflow = false;
                const ICFGNode* callContext = nullptr;
                /// GEP index variable behind this access (null for non-GEP
                /// paths). Used only by the LLM MAY-triage overlay to slice the
                /// surviving loop-induction MAYs; never affects sound emission.
                const SVFVar* indexVar = nullptr;
            };

            std::queue<RangeFlowNode> worklist;
            RangeAnalysis rangeAnalysis;
            HeapAllocationHandler heapAllocationHandler;
            MemCopyAPIRegistry memCopyRegistry;
            SVFBugReport bugReport;
            /// Optional LLM-assisted MAY-triage overlay (pure add-on; does not
            /// alter the sound MUST/MAY classification in bugReport).
            LLMTriage llmTriage;

            /// Fixpoint state: (parent buffer root, base var, k=1 call context) ->
            /// known accumulated offset. The call context keeps the *same*
            /// (parent, base) pair propagated from *different* call sites distinct
            /// (e.g. one buffer passed to one callee from several call sites),
            /// instead of the second call site being swallowed by the "state
            /// unchanged" fixpoint guard. The context set is finite (k=1 call
            /// sites x bounded call depth), so termination is preserved.
            std::map<std::tuple<const SVFVar*, const SVFVar*, const ICFGNode*>, Range> flowState;
            /// Per-variable aggregated buffer membership (latest joined offset).
            std::map<const SVFVar*, BufferInfo> bufferOf;
            /// Symbolic allocation size keyed by the *allocation-result* SVFVar
            /// (direct case: `p = malloc(n); memcpy(p, ...)`). Recorded even when
            /// the numeric size is unknown (TOP), enabling the symbolic
            /// under-allocation check in checkMemoryOps.
            std::map<const SVFVar*, AllocSizeSym> allocResultSym;
            /// Symbolic allocation size keyed by the structural location token of
            /// the address the allocation result is stored into (struct-field
            /// case: `obj->buf = malloc(n); ... memcpy(obj->buf, ...)`), so a
            /// later load of the same field recovers the allocation size even
            /// though the worklist does not connect field store -> load.
            std::map<std::string, AllocSizeSym> allocAddrSym;
            /// Reported (loc,kind,context) keys, for recording-time dedup.
            std::set<std::string> bugLoc;
            /// Deferred reports, resolved/emitted by flushReports().
            std::vector<PendingReport> pendingReports;
    };
}

#endif /* BUFFER_OVERFLOW_CHECKER_H_ */

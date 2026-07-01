//===----- BufferOverflowChecker.cpp -- Detecting Buffer Overflow errors --------------//
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
 * BufferOverflowChecker.cpp
 *
 *  Created on: Nov 10 , 2025
 *      Author: Yaokun Yang
 *
 *  Buffer-overflow / array-out-of-bounds static detector.
 *
 *  Pipeline:
 *    initialize()    -- seed worklist from stack/heap objects and allocation
 *                       call sites (size via AllocAPIRegistry).
 *    propagate()     -- worklist fixpoint over GEP / Copy / Store / Load and
 *                       interprocedural Call/Ret edges (plan A), carrying the
 *                       (buffer-root, accumulated-offset) pair; convergence is
 *                       guaranteed by "enqueue only on state change" + widening.
 *    checkMemoryOps()-- memory-copy / string API write/read overflow checks.
 *
 *  All reports are routed through SVFBugReport (must = FULL / may = PARTIAL),
 *  deduplicated by source location.
 */

#include "BOF/BufferOverflowChecker.h"
#include "SVFIR/SVFIR.h"
#include "Graphs/ICFG.h"
#include "Graphs/ICFGNode.h"
#include "Util/SVFUtil.h"

using namespace SVF;

namespace
{
/// Whether an index/offset range is *effectively* unbounded (degraded toward
/// infinity), i.e. the loop-induction MAY scenario the LLM triage overlay
/// targets. We cannot rely on the strict Range::isTop() here: arithmetic on the
/// saturated sentinels (e.g. negate/sext) can produce NINF+1 / INF-1 rather
/// than the exact NINF / INF constants, so isTop() would miss them even though
/// Range::toString() already renders them as "-INF"/"INF". This mirrors that
/// tolerant rendering and is used only by the read-only overlay (never affects
/// the sound MUST/MAY classification).
[[maybe_unused]] inline bool effectivelyUnbounded(const Range& r)
{
    return r.getUpper() >= Range::INF - 1024 || r.getLower() <= Range::NINF + 1024;
}
} // namespace

/// Interprocedural (plan A) expansion depth bound to prevent divergence.
const uint8_t BufferOverflowChecker::MAX_CALL_DEPTH = 4;

/// Per-buffer cap on the number of call-site contexts used for seeding (Fix B).
const uint8_t BufferOverflowChecker::MAX_SEED_CONTEXTS = 4;

BufferOverflowChecker::BufferOverflowChecker()
        : heapAllocationHandler(&rangeAnalysis) {
}

void BufferOverflowChecker::runOnModule(SVFIR* pag)
{
    assert(pag && "PAG must not be null");

    SVFUtil::outs() << "[BufferOverflowChecker] start buffer overflow analysis\n";

    initialize(pag);
    bindCallArguments(pag);
    propagate(pag);
    checkMemoryOps(pag);
    // Resolve cross-context MUST/MAY priority, then emit.
    flushReports();

    SVFUtil::outs() << "[BufferOverflowChecker] done, "
                    << bugReport.getBugSet().size()
                    << " buffer-overflow bug(s) reported\n";
}

void BufferOverflowChecker::initialize(SVFIR* pag)
{
    // ===== Stack / heap objects modeled by SVF (Addr statements) =====
    SVFStmt::SVFStmtSetTy addrStmtSet = pag->getSVFStmtSet(SVFStmt::Addr);
    for (const auto& stmt : addrStmtSet)
    {
        const auto* addrStmt = SVFUtil::dyn_cast<AddrStmt>(stmt);
        if (!addrStmt)
            continue;

        const SVFVar* src = addrStmt->getRHSVar();
        const SVFVar* dst = addrStmt->getLHSVar();

        // alloca instructions (stack objects, element-domain offsets)
        if (const StackObjVar* stackObjVar = SVFUtil::dyn_cast<StackObjVar>(src))
        {
            // Skip pointer-storage allocas (e.g. `char* p;`): they are not
            // indexable array buffers themselves. The buffer they point to is
            // tracked via the heap/allocation path, and the pointer value is
            // forwarded through the Store/Load chain. Seeding such a 1-element
            // pointer slot as a buffer would otherwise leak a bogus [0,0] range
            // into pointee GEPs and cause false positives.
            if (SVFUtil::isa<SVFPointerType>(stackObjVar->getType()))
                continue;
            if (rangeAnalysis.analyzeBufferRange(stackObjVar))
            {
                enqueue(dst, src, Range(0, 0), false, 0);
                // Fix B: also seed under the enclosing function's call contexts.
                seedBufferUnderCallContexts(pag, dst, src, false,
                                            addrStmt->getICFGNode()->getFun());
            }
        }
        // heap objects modeled by SVF (byte-domain offsets)
        else if (const HeapObjVar* heapObjVar = SVFUtil::dyn_cast<HeapObjVar>(src))
        {
            // Record the *symbolic* allocation size of this heap object, keyed by
            // the result pointer (dst) and by the structural token of any
            // field/slot dst is stored into. External allocators (malloc /
            // calloc / OsalMem*) are modeled as AddrStmt->HeapObjVar (no CallPE),
            // and their numeric size is frequently unknown (TOP), so this side
            // table is what lets checkMemoryOps prove "allocated < copied"
            // off-by-one overflows (BO-4 / BO-5) the numeric domain cannot.
            if (const CallICFGNode* allocCall =
                    SVFUtil::dyn_cast<CallICFGNode>(addrStmt->getICFGNode()))
            {
                AllocSizeSym aSym;
                if (heapAllocationHandler.getAllocSizeOperand(allocCall, aSym) &&
                    aSym.sizeVar)
                    recordAllocSizeSym(dst, aSym);
            }

            if (rangeAnalysis.analyzeBufferRange(heapObjVar))
            {
                enqueue(dst, src, Range(0, 0), true, 0);
                seedBufferUnderCallContexts(pag, dst, src, true,
                                            addrStmt->getICFGNode()->getFun());
            }
        }
    }

    // ===== Allocation call sites (size derived from actual call arguments) =====
    SVFStmt::SVFStmtSetTy callStmtSet = pag->getSVFStmtSet(SVFStmt::Call);
    for (const auto& stmt : callStmtSet)
    {
        const auto* callPE = SVFUtil::dyn_cast<CallPE>(stmt);
        // Exclude thread-fork edges that also match CallPE::classof.
        if (!callPE || callPE->getEdgeKind() != SVFStmt::Call)
            continue;

        const CallICFGNode* callInst = callPE->getCallInst();
        const FunObjVar* funObjVar = callInst->getCalledFunction();
        if (!funObjVar || !heapAllocationHandler.isAllocAPI(funObjVar))
            continue;

        const SVFVar* dst = callPE->getLHSVar();
        Range sizeRange = heapAllocationHandler.analyzeAllocSize(callInst);

        // Record the *symbolic* allocation size (even when its numeric range is
        // unknown / TOP) so checkMemoryOps can later prove "allocated < copied"
        // off-by-one overflows (BO-4 / BO-5) that the numeric domain cannot. The
        // result is keyed both by the allocation-result var (direct buffers) and
        // by the structural token of any field/slot it is stored into (so a
        // later load of that field recovers the size without a store->load edge).
        AllocSizeSym aSym;
        if (heapAllocationHandler.getAllocSizeOperand(callInst, aSym) && aSym.sizeVar)
            recordAllocSizeSym(dst, aSym);

        // Only seed if a positive allocation size could be determined; the
        // conservative (upper-bound) capacity avoids spurious "must" overflows.
        if (sizeRange.getUpper() > 0)
        {
            rangeAnalysis.setBufferRange(dst, Range(0, sizeRange.getUpper() - 1));
            enqueue(dst, dst, Range(0, 0), true, 0);
            // Fix B: also seed under the enclosing function's call contexts so a
            // heap buffer indexed by a callee formal resolves precisely (MUST).
            seedBufferUnderCallContexts(pag, dst, dst, true, callInst->getFun());
        }
    }

    // ===== Allocators recognised by BOF but NOT modeled by SVF as heap =====
    // Platform allocators such as OsalMemCalloc are recognised by the BOF
    // registry yet SVF core does not model them as heap objects (no
    // AddrStmt->HeapObjVar and no CallPE). Their call result is a plain value
    // var. Record the symbolic allocation size keyed by that result var and by
    // the structural token of any field/slot it is stored into, which is what
    // lets checkMemoryOps prove off-by-one under-allocations like BO-5
    // (dst->beaconIe = OsalMemCalloc(len-1); memcpy_s(dst->beaconIe, len, ...)).
    for (const CallICFGNode* callInst : pag->getCallSiteSet())
    {
        const FunObjVar* funObjVar = callInst->getCalledFunction();
        if (!funObjVar || !heapAllocationHandler.isAllocAPI(funObjVar))
            continue;
        const RetICFGNode* retNode = callInst->getRetICFGNode();
        if (!retNode || !pag->callsiteHasRet(retNode))
            continue;
        const SVFVar* ret = retNode->getActualRet();
        // Skip results already recorded via the modeled-heap or CallPE passes.
        if (!ret || allocResultSym.count(ret))
            continue;
        AllocSizeSym aSym;
        if (heapAllocationHandler.getAllocSizeOperand(callInst, aSym) && aSym.sizeVar)
            recordAllocSizeSym(ret, aSym);
    }
}

void BufferOverflowChecker::seedBufferUnderCallContexts(SVFIR* pag, const SVFVar* base,
                                                        const SVFVar* parent, bool isHeap,
                                                        const FunObjVar* enclosingFun)
{
    if (!enclosingFun)
        return;
    // Enumerate the direct call sites of the enclosing function (same ICFG-walk
    // style already used by checkMemoryOps), bounded by MAX_SEED_CONTEXTS. Each
    // such CallICFGNode is the k=1 context at which bindCallArguments has bound
    // this function's scalar formals to their actual ranges.
    uint8_t seeded = 0;
    ICFG* icfg = pag->getICFG();
    for (auto nodeIt = icfg->begin(); nodeIt != icfg->end(); ++nodeIt)
    {
        if (seeded >= MAX_SEED_CONTEXTS)
            break;
        const CallICFGNode* call = SVFUtil::dyn_cast<CallICFGNode>(nodeIt->second);
        if (!call || call->getCalledFunction() != enclosingFun)
            continue;
        enqueue(base, parent, Range(0, 0), isHeap, 0, call);
        ++seeded;
    }
}

void BufferOverflowChecker::bindCallArguments(SVFIR* pag)
{
    // Context-sensitive (k=1) pre-pass: for every direct call's CallPE that maps
    // an actual argument (RHS, in the caller) to a callee scalar-integer formal
    // (LHS), evaluate the actual's range in the caller and bind it to
    // (call-site, formal). The context-sensitive analyzeVarRange then recovers
    // this precise range when the formal is used as an index inside the callee,
    // instead of crossing the Call edge and degrading to TOP.
    SVFStmt::SVFStmtSetTy callStmtSet = pag->getSVFStmtSet(SVFStmt::Call);
    for (const auto& stmt : callStmtSet)
    {
        const auto* callPE = SVFUtil::dyn_cast<CallPE>(stmt);
        // Exclude thread-fork edges that also match CallPE::classof.
        if (!callPE || callPE->getEdgeKind() != SVFStmt::Call)
            continue;

        const SVFVar* formal = callPE->getLHSVar();
        const SVFVar* actual = callPE->getRHSVar();
        if (!formal || !actual)
            continue;
        // Minimal viable version: only scalar (non-pointer) integer formals.
        // Buffer-pointer arguments keep flowing through the worklist as before.
        if (formal->isPointer())
            continue;

        // Evaluate the actual argument's range context-insensitively in the
        // caller (a literal like 11 resolves to [11,11]).
        Range argRange = rangeAnalysis.analyzeVarRange(actual);
        // Only bind a precise, usable range; unknown (TOP/BOTTOM) adds nothing
        // and would mask the context-insensitive fallback.
        if (argRange.isTop() || argRange.isBottom())
            continue;

        rangeAnalysis.bindFormalRange(callPE->getCallInst(), formal, argRange);
    }
}

void BufferOverflowChecker::enqueue(const SVFVar* base, const SVFVar* parent,
                                    const Range& offset, bool isHeap, uint8_t callDepth,
                                    const ICFGNode* callContext)
{
    const auto key = std::make_tuple(parent, base, callContext);
    auto it = flowState.find(key);
    const bool seen = (it != flowState.end());

    Range oldOffset = seen ? it->second : Range::BOTTOM;
    Range joined = Range::join(oldOffset, offset);
    // Widening over loop / recursive / interprocedural back-edges guarantees
    // that the ascending offset chain terminates.
    Range widened = Range::widening(oldOffset, joined);

    // Fixpoint: only (re)enqueue when the abstract state actually changed.
    if (seen && Range::eq(widened, oldOffset))
        return;

    flowState[key] = widened;

    // Aggregate per-variable buffer membership for checkMemoryOps.
    BufferInfo& bi = bufferOf[base];
    if (bi.parent == nullptr)
    {
        bi.parent = parent;
        bi.isHeap = isHeap;
        bi.offset = widened;
    }
    else
    {
        bi.offset = Range::join(bi.offset, widened);
    }

    worklist.push(RangeFlowNode(base, parent, widened, isHeap, callDepth, callContext));
}

void BufferOverflowChecker::propagate(SVFIR*)
{
    while (!worklist.empty())
    {
        RangeFlowNode srcNode = worklist.front();
        worklist.pop();

        for (const auto& svfStmt : srcNode.base->getOutEdges())
        {
            if (const auto* gepStmt = SVFUtil::dyn_cast<GepStmt>(svfStmt))
            {
                handleGep(gepStmt, srcNode);
            }
            else if (const auto* copyStmt = SVFUtil::dyn_cast<CopyStmt>(svfStmt))
            {
                handleCopyLike(copyStmt->getLHSVar(), srcNode, copyStmt->getICFGNode());
            }
            else if (const auto* storeStmt = SVFUtil::dyn_cast<StoreStmt>(svfStmt))
            {
                handleCopyLike(storeStmt->getLHSVar(), srcNode, storeStmt->getICFGNode());
            }
            else if (const auto* loadStmt = SVFUtil::dyn_cast<LoadStmt>(svfStmt))
            {
                handleCopyLike(loadStmt->getLHSVar(), srcNode, loadStmt->getICFGNode());
            }
            // Interprocedural plan A: precisely filter Call/Ret edges so that
            // ThreadFork/ThreadJoin (which also match CallPE/RetPE::classof) are
            // never mistaken for procedure-call/return edges.
            else if (svfStmt->getEdgeKind() == SVFStmt::Call)
            {
                handleCall(SVFUtil::cast<CallPE>(svfStmt), srcNode);
            }
            else if (svfStmt->getEdgeKind() == SVFStmt::Ret)
            {
                handleRet(SVFUtil::cast<RetPE>(svfStmt), srcNode);
            }
        }
    }
}

void BufferOverflowChecker::handleGep(const GepStmt* gepStmt, const RangeFlowNode& srcNode)
{
    const SVFVar* dstVar = gepStmt->getLHSVar();
    const ICFGNode* loc = gepStmt->getICFGNode();

    const AccessPath& ap = gepStmt->getAccessPath();
    const AccessPath::IdxOperandPairs& idxOperandPairs = ap.getIdxOperandPairVec();

    Range total_offset = Range(0);
    const SVFVar* idxVar = nullptr; // first unbounded (TOP) non-const index, for triage
    for (int i = (int)idxOperandPairs.size() - 1; i >= 0; i--)
    {
        const SVFVar* var = idxOperandPairs[i].first;
        const SVFType* type = idxOperandPairs[i].second;

        Range var_offset = rangeAnalysis.analyzeVarRange(var, srcNode.callContext);

        // Soundness guard (root cause of the DEFECT-7 false negative):
        // a non-constant index whose range we genuinely failed to resolve (e.g.
        // a callee formal parameter whose actual value is unknown under this
        // context) collapses to BOTTOM. BOTTOM is the *absorbing* element of the
        // arithmetic below (add/mul) and would make the whole access offset
        // BOTTOM, which checkAccess then treats as a (vacuous) subset of any
        // buffer -> silently in-bounds. That conflates "couldn't compute" with
        // "provably safe". Promote such an unresolved variable index to TOP
        // (unknown) so the access is conservatively reported (at least MAY).
        if (var_offset.isBottom() && !SVFUtil::isa<ConstIntValVar>(var))
            var_offset = Range::TOP;

        // Remember the (first) non-constant index operand behind this access.
        // Previously only *unbounded* (TOP) indices were captured; with the
        // guard/loop narrowing now resolving loop indices to a finite interval
        // (e.g. off-by-one i in [0,10]), the surviving MAY is no longer TOP, so
        // we anchor triage on the first symbolic index regardless of bound. The
        // sound MUST/MAY classification below is unaffected (read-only anchor).
        if (!idxVar && !SVFUtil::isa<ConstIntValVar>(var))
            idxVar = var;

        if (type == nullptr)
        {
            total_offset = Range::add(total_offset, var_offset);
            continue;
        }

        if (srcNode.isHeap)
        {
            // Heap objects are byte-modeled. Accumulate each dimension's byte
            // contribution (fixes the old "overwrite" bug that dropped the
            // already-accumulated offset of earlier dimensions).
            Range dim;
            if (SVFUtil::isa<SVFPointerType>(type))
                dim = Range::mul(var_offset, Range(ap.gepSrcPointeeType()->getByteSize()));
            else
                dim = var_offset;
            total_offset = Range::add(total_offset, dim);
        }
        else if (SVFUtil::isa<SVFPointerType>(type))
        {
            total_offset = Range::add(
                total_offset,
                Range::mul(var_offset, Range(ap.getElementNum(ap.gepSrcPointeeType()))));
        }
        else
        {
            // Struct/array field index: flatten via SVF type info, clamping (and
            // reporting) any field index that escapes the aggregate.
            const std::vector<u32_t>& so =
                PAG::getPAG()->getTypeInfo(type)->getFlattenedElemIdxVec();
            Range type_size = Range(0, (long long)so.size() - 1);
            if (!var_offset.isSubset(type_size))
            {
                // The surviving MAY for an array index `a[i]` is emitted *here*
                // (the field-flatten path), not at the function tail where the
                // offset has already been clamped to the valid range. Pass the
                // triage index so the LLM MAY-triage overlay can slice it.
                checkAccess(dstVar, var_offset, type_size, false, BofKind::GEP_OOB, loc,
                            srcNode.callContext, idxVar);
                var_offset = Range::meet(var_offset, type_size);
            }
            // After clamping, var_offset is within [0, so.size()-1]; guard anyway.
            if (var_offset.isBottom())
                continue;
            total_offset = Range::join(
                total_offset,
                Range(PAG::getPAG()->getFlattenedElemIdx(type, var_offset.getLower()),
                      PAG::getPAG()->getFlattenedElemIdx(type, var_offset.getUpper())));
        }
    }

    Range accumulate_offset = Range::add(total_offset, srcNode.accumulate_offset);
    Range buffer_size = rangeAnalysis.getBufferRange(srcNode.parent);

    checkAccess(dstVar, accumulate_offset, buffer_size, srcNode.isHeap, BofKind::GEP_OOB, loc,
                srcNode.callContext, idxVar);
    enqueue(dstVar, srcNode.parent, accumulate_offset, srcNode.isHeap, srcNode.callDepth,
            srcNode.callContext);
}

void BufferOverflowChecker::handleCopyLike(const SVFVar* dstVar, const RangeFlowNode& srcNode,
                                           const ICFGNode* loc)
{
    Range accumulate_offset = srcNode.accumulate_offset;
    Range buffer_size = rangeAnalysis.getBufferRange(srcNode.parent);

    checkAccess(dstVar, accumulate_offset, buffer_size, srcNode.isHeap, BofKind::GEP_OOB, loc,
                srcNode.callContext);
    enqueue(dstVar, srcNode.parent, accumulate_offset, srcNode.isHeap, srcNode.callDepth,
            srcNode.callContext);
}

void BufferOverflowChecker::handleCall(const CallPE* callPE, const RangeFlowNode& srcNode)
{
    // Plan A: forward the (buffer-root, accumulated-offset) from the actual
    // argument (RHS) to the callee's formal parameter (LHS), bounded by depth.
    if (srcNode.callDepth >= MAX_CALL_DEPTH)
        return;

    const SVFVar* formal = callPE->getLHSVar();
    // k=1 context: tag the forwarded buffer pointer with this call site so that
    // the callee's scalar-integer formals (bound in bindCallArguments) resolve
    // to their precise actual-argument range at index expressions.
    const ICFGNode* ctx = callPE->getCallInst();
    enqueue(formal, srcNode.parent, srcNode.accumulate_offset, srcNode.isHeap,
            (uint8_t)(srcNode.callDepth + 1), ctx);
}

void BufferOverflowChecker::handleRet(const RetPE* retPE, const RangeFlowNode& srcNode)
{
    // Plan A: forward from the callee's formal return (RHS) back to the caller's
    // receiving variable (LHS).
    const SVFVar* recv = retPE->getLHSVar();
    enqueue(recv, srcNode.parent, srcNode.accumulate_offset, srcNode.isHeap,
            srcNode.callDepth, srcNode.callContext);
}

void BufferOverflowChecker::checkAccess(const SVFVar* dstVar, const Range& offset,
                                        const Range& size, bool isHeap,
                                        BofKind kind, const ICFGNode* loc,
                                        const ICFGNode* callContext,
                                        const SVFVar* indexVar)
{
    // Unknown buffer size (object not modeled): nothing to check.
    if (size.isBottom())
        return;
    // Empty / unreachable access offset: nothing to report. NB: a genuinely
    // *unknown* index is promoted to TOP upstream (handleGep), so an offset
    // reaching here as BOTTOM means "unreached", not "unknown" -- it must
    // neither be treated as a vacuous in-bounds subset nor (via the mustOverflow
    // test below, where BOTTOM.lower=INF) be misreported as a must-overflow.
    if (offset.isBottom())
        return;
    // In bounds.
    if (offset.isSubset(size))
        return;

    // must-overflow: the offset is entirely outside the valid range.
    const bool mustOverflow =
        (offset.getLower() > size.getUpper()) || (offset.getUpper() < size.getLower());

    reportBufferOverflowError(dstVar, offset, size, isHeap, kind, loc, mustOverflow,
                              callContext, indexVar);
}

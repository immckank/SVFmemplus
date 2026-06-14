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

#include <algorithm>

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
inline bool effectivelyUnbounded(const Range& r)
{
    return r.getUpper() >= Range::INF - 1024 || r.getLower() <= Range::NINF + 1024;
}

/// Make source-location strings friendlier for end users.
///
/// SVF core (svf-llvm/LLVMUtil.cpp::getSourceLoc) serialises locations with the
/// terse keys "ln" / "cl" / "fl". We must not touch that shared code, so the
/// BOF module post-processes the strings it is about to print, rewriting the
/// keys to their full names: "ln"->"line", "cl"->"col", "fl"->"file".
///
/// Replacement is anchored on the exact quoted-key-plus-colon token (e.g.
/// `"ln":`) so it never matches substrings inside file paths or value names.
std::string friendlyLoc(std::string s)
{
    static const std::pair<const char*, const char*> kRewrites[] = {
        {"\"ln\":", "\"line\":"},
        {"\"cl\":", "\"col\":"},
        {"\"fl\":", "\"file\":"},
    };
    for (const auto& r : kRewrites)
    {
        const std::string from = r.first;
        const std::string to = r.second;
        for (size_t pos = s.find(from); pos != std::string::npos;
             pos = s.find(from, pos + to.size()))
        {
            s.replace(pos, from.size(), to);
        }
    }
    return s;
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

        // Remember the (first) genuinely unbounded non-constant index operand:
        // this is the loop-induction variable behind a surviving MAY, used by
        // the LLM triage overlay to slice the access. Read-only; no effect on
        // the sound classification below.
        // Remember the (first) genuinely unbounded non-constant index operand:
        // this is the loop-induction variable behind a surviving MAY, used by
        // the LLM triage overlay to slice the access. Read-only; no effect on
        // the sound classification below.
        if (!idxVar && !SVFUtil::isa<ConstIntValVar>(var) && effectivelyUnbounded(var_offset))
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

// ===========================================================================
// Feature 2: memory-copy / string API overflow checks.
// ===========================================================================
bool BufferOverflowChecker::getByteBuffer(const SVFVar* var, signed long long& capacity,
                                          Range& byteOffset, bool& isHeap)
{
    auto it = bufferOf.find(var);
    if (it == bufferOf.end())
        return false;

    const BufferInfo& bi = it->second;
    if (bi.parent == nullptr)
        return false;

    Range bufRange = rangeAnalysis.getBufferRange(bi.parent);
    if (bufRange.isBottom())
        return false;

    const signed long long numUnits = bufRange.getUpper() + 1;
    if (numUnits <= 0)
        return false;

    isHeap = bi.isHeap;
    if (bi.isHeap)
    {
        // Heap buffers are byte-modeled: valid range [0, numUnits-1] is bytes.
        capacity = numUnits;
        byteOffset = bi.offset;
    }
    else
    {
        // Stack buffers carry element-domain offsets; convert to bytes.
        const SVFType* pty = bi.parent->getType();
        const signed long long totalBytes = pty ? (signed long long)pty->getByteSize() : numUnits;
        signed long long elemByte = totalBytes / numUnits;
        if (elemByte <= 0)
            elemByte = 1;
        capacity = totalBytes;
        byteOffset = Range::mul(bi.offset, Range(elemByte));
    }
    return true;
}

// ===========================================================================
// Symbolic under-allocation check: prove "allocated < copied" under TOP ranges
// (BO-4: malloc(len-1) then memcpy_s(buf,len,..,len);
//  BO-5: OsalMemCalloc(beaconIeLen-1) then memcpy_s(beaconIe,beaconIeLen,..)).
// ===========================================================================

// Record an allocation's symbolic size, keyed both by the allocation-result var
// (direct buffers) and by the structural token of any field/slot it is stored
// into (so a later load of that field recovers the size without an explicit
// store->load edge). Shared by every allocation-recording pass in initialize().
void BufferOverflowChecker::recordAllocSizeSym(const SVFVar* resultVar,
                                               const AllocSizeSym& aSym)
{
    if (!resultVar)
        return;
    allocResultSym[resultVar] = aSym;
    for (const SVFStmt* e : resultVar->getOutEdges())
        if (const StoreStmt* st = SVFUtil::dyn_cast<StoreStmt>(e))
            if (st->getRHSVar() == resultVar)
                allocAddrSym[rangeAnalysis.locationToken(st->getLHSVar())] = aSym;
}

bool BufferOverflowChecker::findAllocSizeForBuffer(const SVFVar* bufArg,
                                                   AllocSizeSym& out)
{
    // Walk a short Copy/Load chain from the buffer argument back to either the
    // allocation-result var (direct buffers) or the struct-field/slot the
    // allocation result was stored into (matched by structural location token).
    const SVFVar* v = bufArg;
    for (int i = 0; i < 16 && v; ++i)
    {
        auto dit = allocResultSym.find(v);
        if (dit != allocResultSym.end())
        {
            out = dit->second;
            return true;
        }

        // Loaded from an address: match the recorded allocation-result store.
        if (v->hasIncomingEdges(SVFStmt::PEDGEK::Load))
        {
            for (auto it = v->getIncomingEdgesBegin(SVFStmt::PEDGEK::Load);
                 it != v->getIncomingEdgesEnd(SVFStmt::PEDGEK::Load); ++it)
                if (const LoadStmt* ld = SVFUtil::dyn_cast<LoadStmt>(*it))
                {
                    auto ait = allocAddrSym.find(
                        rangeAnalysis.locationToken(ld->getRHSVar()));
                    if (ait != allocAddrSym.end())
                    {
                        out = ait->second;
                        return true;
                    }
                }
            break;
        }

        // Step through a single forwarding copy and retry.
        const SVFVar* next = nullptr;
        if (v->hasIncomingEdges(SVFStmt::PEDGEK::Copy))
            for (auto it = v->getIncomingEdgesBegin(SVFStmt::PEDGEK::Copy);
                 it != v->getIncomingEdgesEnd(SVFStmt::PEDGEK::Copy); ++it)
                if (const CopyStmt* cp = SVFUtil::dyn_cast<CopyStmt>(*it))
                {
                    next = cp->getRHSVar();
                    break;
                }
        if (!next)
            break;
        v = next;
    }
    return false;
}

void BufferOverflowChecker::trySymbolicUnderAlloc(const SVFVar* bufArg,
                                                  const SVFVar* lenVar,
                                                  signed long long byteOffUpper,
                                                  const ICFGNode* loc)
{
    if (!bufArg || !lenVar)
        return;

    AllocSizeSym sym;
    if (!findAllocSizeForBuffer(bufArg, sym) || !sym.sizeVar)
        return;
    // calloc-style (element*count) sizes are in element units; the copy-length
    // APIs handled here are byte-based, so restrict to the malloc-style
    // (factor==1, non-elem) case to remain strictly false-positive-free.
    if (sym.isElemMul)
        return;

    const std::vector<AffineTerm> allocTerms = rangeAnalysis.analyzeAffine(sym.sizeVar);
    const std::vector<AffineTerm> copyTerms  = rangeAnalysis.analyzeAffine(lenVar);
    if (allocTerms.empty() || copyTerms.empty())
        return;

    // Capacity == allocation size; the buffer holds bytes
    // [byteOffUpper, byteOffUpper + len - 1]. With a shared symbolic base the
    // base cancels, so overflow iff  byteOffUpper + copyOffset > allocOffset.
    bool matched = false, anyOverflow = false, allOverflow = true;
    long long provenOver = 0;
    for (const AffineTerm& c : copyTerms)
    {
        if (c.base.empty())   // a pure constant copy length is the numeric path's job
            continue;
        long long minA = 0, maxA = 0;
        bool haveA = false;
        for (const AffineTerm& a : allocTerms)
        {
            if (a.base != c.base)
                continue;
            if (!haveA) { minA = maxA = a.offset; haveA = true; }
            else { minA = std::min(minA, a.offset); maxA = std::max(maxA, a.offset); }
        }
        if (!haveA)
            continue;
        matched = true;
        // Worst case (this copy vs the smallest matching allocation) -> existence.
        if (byteOffUpper + c.offset > minA)
        {
            anyOverflow = true;
            provenOver = std::max(provenOver, byteOffUpper + c.offset - minA);
        }
        // Best case (this copy vs the largest matching allocation) decides MUST.
        if (!(byteOffUpper + c.offset > maxA))
            allOverflow = false;
    }

    if (!matched || !anyOverflow)
        return;

    // Symbolic sizes have no concrete capacity; surface a representative
    // interval encoding the proven shortfall. Severity carries the real verdict:
    // MUST when every allocation branch overflows, else MAY (conditional
    // under-allocation, matching the defects' "complex branch" semantics).
    const bool mustOverflow = allOverflow;
    Range valid(0, byteOffUpper);
    Range access(0, byteOffUpper + provenOver);
    reportBufferOverflowError(bufArg, access, valid, /*isHeap*/ true,
                              BofKind::MEMCPY_OOB, loc, mustOverflow);
}

void BufferOverflowChecker::checkMemoryOps(SVFIR* pag)
{
    ICFG* icfg = pag->getICFG();
    for (auto nodeIt = icfg->begin(); nodeIt != icfg->end(); ++nodeIt)
    {
        const CallICFGNode* call = SVFUtil::dyn_cast<CallICFGNode>(nodeIt->second);
        if (!call)
            continue;

        const FunObjVar* fun = call->getCalledFunction();
        if (!fun) // indirect call
            continue;

        // Normalize LLVM mem-intrinsic names to their libc counterparts. On
        // Linux/-O0 (where _FORTIFY_SOURCE is inactive) clang lowers
        // memcpy/memmove/memset to the intrinsics llvm.memcpy.p0.p0.i64 /
        // llvm.memmove.p0.p0.i64 / llvm.memset.p0.i64, whereas macOS SDK headers
        // emit the *_chk symbols. The intrinsic argument layout matches the libc
        // rule indices exactly -- memcpy/memmove(dst=0, src=1, len=2, isvol=3),
        // memset(dst=0, val=1, len=2, isvol=3) -- so the same rules apply once
        // the type-suffixed intrinsic name is mapped back. Without this the same
        // source (e.g. memcpy_oob.c) is silently un-checked on Linux.
        const std::string rawName = fun->getName();
        std::string fname = rawName;
        if (rawName.compare(0, 11, "llvm.memcpy") == 0)
            fname = "memcpy";
        else if (rawName.compare(0, 12, "llvm.memmove") == 0)
            fname = "memmove";
        else if (rawName.compare(0, 11, "llvm.memset") == 0)
            fname = "memset";

        const std::vector<MemCopyRule>* ruleSet = memCopyRegistry.getRules(fname);
        if (!ruleSet)
            continue;

        const bool strLike = memCopyRegistry.isStrCopyLike(fname);
        const ICFGNode* loc = call;
        const u32_t argn = call->arg_size();

        for (const MemCopyRule& rule : *ruleSet)
        {
            if (rule.bufArgIdx < 0 || (u32_t)rule.bufArgIdx >= argn)
                continue;

            const SVFVar* bufArg = call->getArgument(rule.bufArgIdx);

            // --- Symbolic under-allocation remediation (BO-4 / BO-5) ---
            // Runs independently of getByteBuffer: the destination buffer is
            // frequently loaded from a struct field that the worklist does not
            // connect (so getByteBuffer fails) and/or the copy length is TOP, yet
            // the allocation size can still be related to the copy length
            // symbolically. Only fires on explicit-length copies.
            if (!strLike && rule.lenArgIdx >= 0 && (u32_t)rule.lenArgIdx < argn)
            {
                signed long long symByteOff = 0;
                signed long long capTmp = 0;
                Range offTmp;
                bool heapTmp = false;
                if (getByteBuffer(bufArg, capTmp, offTmp, heapTmp) &&
                    !offTmp.isBottom() && offTmp.getUpper() >= 0)
                    symByteOff = offTmp.getUpper();
                trySymbolicUnderAlloc(bufArg, call->getArgument(rule.lenArgIdx),
                                      symByteOff, loc);
            }

            signed long long capacity = 0;
            Range byteOffset;
            bool isHeap = false;
            if (!getByteBuffer(bufArg, capacity, byteOffset, isHeap))
                continue;

            // Determine the number of bytes accessed via this buffer argument.
            Range lenBytes;
            if (strLike)
            {
                // strcpy/strcat-like: copied length is bounded by the source
                // string buffer's remaining capacity (including terminating nul).
                const u32_t srcIdx = (u32_t)rule.bufArgIdx + 1;
                if (srcIdx >= argn)
                    continue;
                signed long long srcCap = 0;
                Range srcOff;
                bool srcHeap = false;
                if (!getByteBuffer(call->getArgument(srcIdx), srcCap, srcOff, srcHeap))
                    continue; // unknown source length -> stay silent (no noise)
                const signed long long maxLen = srcCap - srcOff.getLower();
                if (maxLen <= 0)
                    continue;
                lenBytes = Range(1, maxLen);
            }
            else
            {
                if (rule.lenArgIdx < 0 || (u32_t)rule.lenArgIdx >= argn)
                    continue;
                lenBytes = rangeAnalysis.analyzeVarRange(call->getArgument(rule.lenArgIdx));
                // Unknown length -> avoid false-positive flooding.
                if (lenBytes.isTop() || lenBytes.getUpper() <= 0)
                    continue;
            }

            // Highest accessed byte index = byteOffset + (len - 1).
            const signed long long lenUpper = lenBytes.getUpper();
            Range accessRange = Range::add(byteOffset, Range(0, lenUpper - 1));
            Range valid(0, capacity - 1);
            if (accessRange.isSubset(valid))
                continue;

            const signed long long lenLower = lenBytes.getLower() > 0 ? lenBytes.getLower() : 0;
            const bool mustOverflow =
                (byteOffset.getLower() > valid.getUpper()) ||
                (lenLower >= 1 &&
                 Range::add(Range(byteOffset.getLower()), Range(lenLower - 1)).getLower()
                     > valid.getUpper());

            BofKind kind;
            if (fname == "memset" || fname == "__memset_chk" ||
                fname == "wmemset" || fname == "bzero")
                kind = BofKind::MEMSET_OOB;
            else if (strLike)
                kind = BofKind::STRCPY_OOB;
            else
                kind = BofKind::MEMCPY_OOB;

            reportBufferOverflowError(bufArg, accessRange, valid, isHeap, kind, loc, mustOverflow);
        }
    }
}

// ===========================================================================
// Reporting: deduplicated by source location, routed through SVFBugReport.
// ===========================================================================
static const char* bofKindStr(BofKind kind)
{
    switch (kind)
    {
    case BofKind::GEP_OOB:    return "array/pointer access";
    case BofKind::MEMCPY_OOB: return "memcpy/memmove";
    case BofKind::MEMSET_OOB: return "memset";
    case BofKind::STRCPY_OOB: return "strcpy/strcat";
    }
    return "access";
}

void BufferOverflowChecker::reportBufferOverflowError(const SVFVar* base, const Range& offset,
                                                      const Range& size, bool isHeap,
                                                      BofKind kind, const ICFGNode* loc,
                                                      bool mustOverflow,
                                                      const ICFGNode* callContext,
                                                      const SVFVar* indexVar)
{
    // Record-time dedup by (source location, kind, call context). The call
    // context (k=1 call-site, null for intraprocedural accesses) keeps the
    // *same* callee instruction reached under *different* contexts distinct
    // (e.g. write_at(a,11) and write_at(a,10) both surface at `p[idx]`), so
    // each per-context verdict is captured once. Final cross-context dedup
    // (suppressing a MAY when the same access point also yields a MUST) is
    // performed later in flushReports().
    if (loc)
    {
        std::string key = loc->getSourceLoc() + "#" + std::to_string((int)kind);
        if (callContext)
            key += "@" + std::to_string(callContext->getId());
        if (!bugLoc.insert(key).second)
            return;
    }

    PendingReport pr;
    pr.base = base;
    pr.offset = offset;
    pr.size = size;
    pr.isHeap = isHeap;
    pr.kind = kind;
    pr.loc = loc;
    pr.mustOverflow = mustOverflow;
    pr.callContext = callContext;
    pr.indexVar = indexVar;
    pendingReports.push_back(pr);
}

void BufferOverflowChecker::flushReports()
{
    // First pass: collect every access point (source location + kind) at which
    // some calling context proves a MUST overflow.
    std::set<std::string> mustPoints;
    for (const PendingReport& pr : pendingReports)
    {
        if (pr.mustOverflow && pr.loc)
            mustPoints.insert(pr.loc->getSourceLoc() + "#" + std::to_string((int)pr.kind));
    }

    // Second pass: emit, with two layers of dedup:
    //  (a) suppress a MAY at any access point that also has a MUST (avoid
    //      double-reporting the same point, e.g. the null-context MAY alongside
    //      the call-site-context MUST -- keep the higher-information MUST);
    //  (b) collapse exact-duplicate verdicts (same loc/kind/severity and same
    //      offset interval) arising from seeding one buffer under several call
    //      contexts that all compute the *same* index. Genuinely distinct
    //      per-context verdicts (different offset intervals, e.g. write_at(a,11)
    //      vs write_at(a,10)) are preserved.
    std::set<std::string> emitted;
    // Surviving loop-induction MAYs eligible for LLM triage: slice id -> the
    // friendly source location (for the terminal annotation pass below).
    std::vector<std::pair<std::string, std::string>> triagedMays;
    for (const PendingReport& pr : pendingReports)
    {
        if (pr.loc)
        {
            const std::string point =
                pr.loc->getSourceLoc() + "#" + std::to_string((int)pr.kind);
            if (!pr.mustOverflow && mustPoints.count(point))
                continue; // suppressed in favour of the MUST at this point

            // Content key: same point + severity + offset interval => duplicate.
            const std::string contentKey =
                point + (pr.mustOverflow ? "!MUST" : "!MAY") +
                "[" + std::to_string(pr.offset.getLower()) + "," +
                std::to_string(pr.offset.getUpper()) + "]";
            if (!emitted.insert(contentKey).second)
                continue;
        }

        // Structured report (requires a non-empty event stack).
        if (pr.loc)
        {
            GenericBug::EventStack eventStack;
            eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, pr.loc));
            bugReport.addAbsExecBug(
                pr.mustOverflow ? GenericBug::FULLBUFOVERFLOW : GenericBug::PARTIALBUFOVERFLOW,
                eventStack, pr.size.getLower(), pr.size.getUpper(),
                pr.offset.getLower(), pr.offset.getUpper());
        }

        // Human-readable terminal output.
        SVFUtil::outs() << "[BufferOverflowChecker] "
                        << (pr.mustOverflow ? "MUST" : "MAY") << " buffer overflow ("
                        << bofKindStr(pr.kind) << ")\n"
                        << "  Base       : " << friendlyLoc(pr.base->toString()) << "\n"
                        << "  Access     : " << pr.offset.toString() << "\n"
                        << "  Valid range: " << pr.size.toString()
                        << (pr.isHeap ? "  (bytes)\n" : "  (elements)\n");
        if (pr.loc)
            SVFUtil::outs() << "  Location   : " << friendlyLoc(pr.loc->getSourceLoc()) << "\n";
        SVFUtil::outs() << "\n";

        // LLM MAY-triage overlay (pure add-on): slice surviving loop-induction
        // MAYs whose index degraded to TOP. Read-only over the IR; the sound
        // report emitted above is untouched.
        if (!pr.mustOverflow && pr.kind == BofKind::GEP_OOB && effectivelyUnbounded(pr.offset) &&
            pr.indexVar && pr.loc)
        {
            BofSlice slice;
            if (llmTriage.collectSlice(pr.base, pr.indexVar, pr.size, pr.isHeap,
                                       pr.loc, rangeAnalysis, slice))
            {
                llmTriage.addSlice(slice);
                triagedMays.emplace_back(slice.id, friendlyLoc(pr.loc->getSourceLoc()));
            }
        }
    }

    // ===== LLM MAY-triage tail: always export slices; optionally annotate. ====
    if (!llmTriage.empty())
    {
        if (llmTriage.writeSlices())
            SVFUtil::outs() << "[LLMTriage] exported " << llmTriage.size()
                            << " slice(s) to " << llmTriage.config().sliceOutPath
                            << "\n";

        std::map<std::string, LLMVerdict> verdicts;
        if (llmTriage.config().hasApi() && llmTriage.runSidecarAndLoad(verdicts))
        {
            const double thr = llmTriage.config().threshold;
            for (const auto& tm : triagedMays)
            {
                auto it = verdicts.find(tm.first);
                if (it == verdicts.end())
                    continue;
                const LLMVerdict& v = it->second;
                // SOUNDNESS: only *upgrade* MAY -> suspected overflow. Never
                // clear to SAFE; SAFE merely lowers display priority.
                if (v.verdict == "OUT_OF_BOUNDS" && v.confidence >= thr)
                {
                    SVFUtil::outs() << "[LLMTriage] LLM_SUSPECT (overflow, conf="
                                    << v.confidence << ") at " << tm.second
                                    << "\n    max_index_reasoned: " << v.maxIndexReasoned
                                    << "\n    rationale: " << v.rationale << "\n\n";
                }
                else if (v.verdict == "SAFE")
                {
                    SVFUtil::outs() << "[LLMTriage] LLM hint SAFE (display only, MAY "
                                       "retained, conf=" << v.confidence << ") at "
                                    << tm.second << "\n\n";
                }
            }
        }
        else if (!llmTriage.config().hasApi())
        {
            SVFUtil::outs() << "[LLMTriage] no LLM endpoint configured; slices "
                               "exported for manual review only.\n";
        }
    }
}

void BufferOverflowChecker::dumpReport(const std::string& filePath)
{
    bugReport.dumpToJsonFile(filePath);
}

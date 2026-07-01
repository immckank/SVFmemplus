//===- BofMemCopyChecks.cpp -- memcpy/string-API & symbolic under-alloc ---===//
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
 *  BofMemCopyChecks.cpp
 *
 *  Feature 2 of BufferOverflowChecker: memory-copy / string-API write/read
 *  overflow checks (memcpy/memmove/memset/strcpy/strcat ...), plus the
 *  symbolic under-allocation check that proves "allocated < copied" under TOP
 *  ranges via analyzeAffine. Split out of BufferOverflowChecker.cpp (the class
 *  declaration / state lives in BOF/BufferOverflowChecker.h; this is purely an
 *  additional translation unit).
 *
 *  Created on: Nov 10 , 2025
 *      Author: Yaokun Yang
 */

#include "BOF/BufferOverflowChecker.h"
#include "SVFIR/SVFIR.h"
#include "Graphs/ICFG.h"
#include "Graphs/ICFGNode.h"
#include "Util/SVFUtil.h"

#include <algorithm>

using namespace SVF;

// ===========================================================================
// String length-oracle classifiers (std::string support / strlen-guard model).
//
// A strcpy/strcat source is frequently *not* a tracked char[] buffer -- it is a
// field of an unknown incoming pointer (`f->d_name`) or the return of
// std::string::data()/c_str(). In those cases the copied length is instead
// bounded by a dominating "length oracle" (strlen(P) or std::string::size())
// whose guarded value (e.g. `> 32`) is recovered via guardedValueRange.
// ===========================================================================
namespace
{
inline bool nameIsStrlen(const std::string& n)
{
    return n == "strlen" || n == "strnlen" || n == "__strlen_chk";
}
// libstdc++ (_ZNSt7__cxx11...) and libc++ (_ZNSt3__1...) both length-prefix the
// selector in Itanium mangling, so matching "basic_string" + the length-tagged
// method token is portable across standard libraries.
inline bool nameIsStrSize(const std::string& n)
{
    return n.find("basic_string") != std::string::npos &&
           (n.find("4size") != std::string::npos ||
            n.find("6length") != std::string::npos);
}
inline bool nameIsStrData(const std::string& n)
{
    return n.find("basic_string") != std::string::npos &&
           (n.find("4data") != std::string::npos ||
            n.find("5c_str") != std::string::npos);
}
} // namespace

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

    // ---- Length-oracle pre-pass (std::string / strlen-guard modeling) ----
    // Index every length oracle so a later strcpy/strcat can bound its source's
    // copied length even when the source is not a tracked char[] buffer:
    //   * strlen(P)                    -> keyed by locationToken(P)   (char*)
    //   * std::string::size()/length() -> keyed by "S(" locationToken(this) ")"
    // and remember which SSA values are std::string::data()/c_str() returns (the
    // usual strcpy source) so they can be bridged to the size() oracle on the
    // same string object. The oracle's return value is guard-refined at the copy
    // site (guardedValueRange), turning `size()>32` into a [33, INF) lower bound.
    std::map<std::string, const SVFVar*> lenOracleByPtr;  // strlen arg tok -> ret
    std::map<std::string, const SVFVar*> lenOracleByStr;  // size 'this' key -> ret
    std::map<const SVFVar*, std::string> dataRetToStr;    // data()/c_str() ret -> key
    for (auto oit = icfg->begin(); oit != icfg->end(); ++oit)
    {
        const CallICFGNode* c = SVFUtil::dyn_cast<CallICFGNode>(oit->second);
        if (!c)
            continue;
        const FunObjVar* f = c->getCalledFunction();
        if (!f || c->arg_size() < 1)
            continue;
        const std::string nm = f->getName();
        const bool isLen  = nameIsStrlen(nm);
        const bool isSize = nameIsStrSize(nm);
        const bool isData = nameIsStrData(nm);
        if (!isLen && !isSize && !isData)
            continue;

        const SVFVar* subject = c->getArgument(0); // char* (strlen) or 'this' (string)
        if (!subject)
            continue;
        const std::string tok = rangeAnalysis.locationToken(subject);
        if (tok.empty())
            continue;

        const RetICFGNode* rn = c->getRetICFGNode();
        if (!rn || !pag->callsiteHasRet(rn))
            continue;
        const SVFVar* ret = rn->getActualRet();
        if (!ret)
            continue;

        if (isData)
            dataRetToStr.emplace(ret, "S(" + tok + ")");
        else if (isLen)
            lenOracleByPtr.emplace(tok, ret);
        else // isSize
            lenOracleByStr.emplace("S(" + tok + ")", ret);
    }

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
                // strcpy/strcat-like: the copied length is strlen(src)+1 bytes.
                const u32_t srcIdx = (u32_t)rule.bufArgIdx + 1;
                if (srcIdx >= argn)
                    continue;
                const SVFVar* srcVar = call->getArgument(srcIdx);
                if (!srcVar)
                    continue;

                bool haveLen = false;

                // (1) Guarded length oracle on the same source. Gives a *lower*
                //     bound (e.g. from `size()>32` / `strlen(d_name)>32`), which
                //     is what can upgrade a strcpy to a MUST overflow.
                Range oracleChars = Range::TOP;
                auto dit = dataRetToStr.find(srcVar); // std::string data()/c_str()
                if (dit != dataRetToStr.end())
                {
                    auto oit = lenOracleByStr.find(dit->second);
                    if (oit != lenOracleByStr.end())
                        oracleChars = rangeAnalysis.guardedValueRange(oit->second, call);
                }
                if (oracleChars.isTop()) // char* source guarded by strlen(src)
                {
                    auto oit = lenOracleByPtr.find(rangeAnalysis.locationToken(srcVar));
                    if (oit != lenOracleByPtr.end())
                        oracleChars = rangeAnalysis.guardedValueRange(oit->second, call);
                }
                if (!oracleChars.isBottom() && !oracleChars.isTop())
                {
                    signed long long lo = oracleChars.getLower();
                    if (lo < 0)
                        lo = 0;
                    const signed long long hi = oracleChars.getUpper();
                    const signed long long bLo =
                        (lo >= Range::INF - 1024) ? Range::INF : lo + 1;
                    const signed long long bHi =
                        (hi >= Range::INF - 1024) ? Range::INF : hi + 1;
                    lenBytes = Range(bLo < 1 ? 1 : bLo, bHi);
                    haveLen = true;
                }

                // (2) Source-buffer capacity (upper bound). Original behaviour,
                //     and the fallback when no guard applies; also tightens the
                //     oracle's upper bound when both are known.
                signed long long srcCap = 0;
                Range srcOff;
                bool srcHeap = false;
                if (getByteBuffer(srcVar, srcCap, srcOff, srcHeap))
                {
                    const signed long long maxLen = srcCap - srcOff.getLower();
                    if (maxLen > 0)
                    {
                        const Range capRange(1, maxLen);
                        if (!haveLen)
                        {
                            lenBytes = capRange;
                            haveLen = true;
                        }
                        else
                        {
                            const Range m = Range::meet(lenBytes, capRange);
                            if (!m.isBottom())
                                lenBytes = m;
                        }
                    }
                }

                if (!haveLen)
                    continue; // unknown source length -> stay silent (no noise)
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

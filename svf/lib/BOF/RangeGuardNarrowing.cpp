//===- RangeGuardNarrowing.cpp -- Guard-/loop-aware range narrowing -------===//
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
 *  RangeGuardNarrowing.cpp
 *
 *  Guard- / loop-aware narrowing of memory loads ("query-time virtual
 *  pi-nodes"): the RangeAnalysis members that, at -O0 where each *use* of a
 *  scalar local is a distinct SSA load value, soundly narrow an individual
 *  load's range by the branch predicate(s) that dominate it (straight-line
 *  if-guards) and, inside loops, by the recognised monotone induction
 *  [init, guard-bound] -- all without materialising any pi-node in the PAG.
 *
 *  Split out of RangeAnalysis.cpp (the class declaration / state lives in
 *  BOF/RangeAnalysis.h; this is purely an additional translation unit).
 *
 *  Created on: Mar 15 , 2025
 *      Author: Yaokun Yang
 */

#include "BOF/RangeAnalysis.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/SVFVariables.h"
#include "Graphs/ICFGNode.h"

using namespace SVF;
using namespace std;

// ===========================================================================
// Local helpers (file-private predicate / interval algebra)
// ===========================================================================
namespace {

/// Saturating decrement/increment of an interval bound (keep INF/NINF sticky).
inline Range::BoundType decBound(Range::BoundType u)
{
    return (u <= Range::NINF + 1024) ? u : u - 1;
}
inline Range::BoundType incBound(Range::BoundType l)
{
    return (l >= Range::INF - 1024) ? l : l + 1;
}

/// Canonical relational comparator of "index OP bound".
enum class Cmp { LT, LE, GT, GE, EQ, NE, UNK };

Cmp predToCmp(u32_t p)
{
    switch (p)
    {
    case CmpStmt::ICMP_SLT: case CmpStmt::ICMP_ULT: return Cmp::LT;
    case CmpStmt::ICMP_SLE: case CmpStmt::ICMP_ULE: return Cmp::LE;
    case CmpStmt::ICMP_SGT: case CmpStmt::ICMP_UGT: return Cmp::GT;
    case CmpStmt::ICMP_SGE: case CmpStmt::ICMP_UGE: return Cmp::GE;
    case CmpStmt::ICMP_EQ:                          return Cmp::EQ;
    case CmpStmt::ICMP_NE:                          return Cmp::NE;
    default:                                        return Cmp::UNK;
    }
}

/// Swap operands: "a OP b"  <=>  "b OP' a".
Cmp swapCmp(Cmp c)
{
    switch (c)
    {
    case Cmp::LT: return Cmp::GT;
    case Cmp::LE: return Cmp::GE;
    case Cmp::GT: return Cmp::LT;
    case Cmp::GE: return Cmp::LE;
    default:      return c; // EQ / NE / UNK
    }
}

/// Logical negation of the comparator (taken on the false edge of a branch).
Cmp negCmp(Cmp c)
{
    switch (c)
    {
    case Cmp::LT: return Cmp::GE;
    case Cmp::LE: return Cmp::GT;
    case Cmp::GT: return Cmp::LE;
    case Cmp::GE: return Cmp::LT;
    case Cmp::EQ: return Cmp::NE;
    case Cmp::NE: return Cmp::EQ;
    default:      return Cmp::UNK;
    }
}

/// Sound interval the index must lie in, given a branch predicate
/// `op0 <pred> op1`, whether the index is op0, the truth value taken on the
/// guarded edge, and the (already computed) range of the bound operand.
/// Returns TOP when no useful one-sided bound can be derived.
Range evalGuardInterval(u32_t pred, bool idxIsOp0, bool condTrue, const Range& bnd)
{
    if (bnd.isBottom() || bnd.isTop())
        return Range::TOP;
    Cmp c = predToCmp(pred);
    if (c == Cmp::UNK)
        return Range::TOP;
    if (!idxIsOp0)
        c = swapCmp(c);
    if (!condTrue)
        c = negCmp(c);

    const Range::BoundType lo = bnd.getLower();
    const Range::BoundType up = bnd.getUpper();
    switch (c)
    {
    case Cmp::LT: return Range(Range::NINF, decBound(up));
    case Cmp::LE: return Range(Range::NINF, up);
    case Cmp::GT: return Range(incBound(lo), Range::INF);
    case Cmp::GE: return Range(lo, Range::INF);
    case Cmp::EQ: return Range(lo, up);
    default:      return Range::TOP; // NE / UNK: no contiguous narrowing
    }
}

} // namespace

void RangeAnalysis::buildGuardIndex()
{
    if (guardsBuilt)
        return;
    guardsBuilt = true;

    SVFIR* pag = SVFIR::getPAG();

    // Map each comparison's result var to the CmpStmt, so a conditional branch
    // can resolve the predicate behind its boolean condition.
    std::map<NodeID, const CmpStmt*> cmpByRes;
    for (SVFStmt* s : pag->getSVFStmtSet(SVFStmt::Cmp))
        if (const CmpStmt* c = SVFUtil::dyn_cast<CmpStmt>(s))
            if (c->getOpVarNum() >= 2 && c->getRes())
                cmpByRes[c->getRes()->getId()] = c;

    for (SVFStmt* s : pag->getSVFStmtSet(SVFStmt::Branch))
    {
        const BranchStmt* br = SVFUtil::dyn_cast<BranchStmt>(s);
        if (!br || !br->isConditional())
            continue;
        const SVFVar* cond = br->getCondition();
        if (!cond)
            continue;
        auto cit = cmpByRes.find(cond->getId());
        if (cit == cmpByRes.end())
            continue;
        const CmpStmt* cmp = cit->second;
        const SVFVar* op0 = cmp->getOpVar(0);
        const SVFVar* op1 = cmp->getOpVar(1);
        const std::string t0 = locationToken(op0);
        const std::string t1 = locationToken(op1);

        const ICFGNode* brNode = br->getICFGNode();
        const FunObjVar* fun = brNode ? brNode->getFun() : nullptr;
        const SVFBasicBlock* brBB = brNode ? brNode->getBB() : nullptr;
        if (!fun || !brBB)
            continue;

        for (u32_t i = 0; i < br->getNumSuccessors(); ++i)
        {
            const ICFGNode* succ = br->getSuccessor(i);
            const SVFBasicBlock* succBB = succ ? succ->getBB() : nullptr;
            if (!succBB)
                continue;
            const bool condTrue = (br->getSuccessorCondValue(i) != 0);

            if (!t0.empty())
            {
                GuardEntry g{cmp->getPredicate(), true, op1, succBB, brBB,
                             condTrue, fun};
                guardIndex[t0].push_back(g);
            }
            if (!t1.empty() && t1 != t0)
            {
                GuardEntry g{cmp->getPredicate(), false, op0, succBB, brBB,
                             condTrue, fun};
                guardIndex[t1].push_back(g);
            }
        }
    }

    for (SVFStmt* s : pag->getSVFStmtSet(SVFStmt::Store))
        if (const StoreStmt* st = SVFUtil::dyn_cast<StoreStmt>(s))
        {
            const std::string at = locationToken(st->getLHSVar());
            if (!at.empty())
                slotStores[at].push_back(st);
        }
}

Range RangeAnalysis::refineLoad(const SVFVar* loadVar, const Range& current,
                                const ICFGNode* context)
{
    if (!guardEnabled || !loadVar)
        return current;
    if (!loadVar->hasIncomingEdges(SVFStmt::PEDGEK::Load))
        return current;

    // Recover the (single) defining load -> address slot + program point.
    const LoadStmt* ld = nullptr;
    int nload = 0;
    for (auto it = loadVar->getIncomingEdgesBegin(SVFStmt::PEDGEK::Load);
         it != loadVar->getIncomingEdgesEnd(SVFStmt::PEDGEK::Load); ++it)
        if (const LoadStmt* l = SVFUtil::dyn_cast<LoadStmt>(*it)) { ld = l; ++nload; }
    if (!ld || nload != 1)
        return current; // ambiguous merge -> stay sound

    const ICFGNode* n = ld->getICFGNode();
    const SVFBasicBlock* loadBB = n ? n->getBB() : nullptr;
    const FunObjVar* fun = n ? n->getFun() : nullptr;
    if (!loadBB || !fun)
        return current;

    // Cheap gate: outside loops only bother while the range is still
    // effectively unbounded (the case that actually causes false-positive MAY
    // floods). INSIDE a loop the naive def-driven range is *polluted* by the
    // back-edge step store (e.g. `i = i + 1` makes the slot resolve to
    // [0, N] instead of the in-body [0, N-1]), so it looks finite yet is one
    // past the true induction bound -- we must still narrow it via the guard.
    // The meet below only ever tightens, so attempting is always sound.
    const bool unbounded = current.getUpper() >= Range::INF - 1024 ||
                           current.getLower() <= Range::NINF + 1024;
    const bool inLoop = fun->hasLoopInfo(loadBB);
    if (!unbounded && !inLoop)
        return current;

    buildGuardIndex();
    const std::string valTok = locationToken(loadVar);
    const std::string addrTok = locationToken(ld->getRHSVar());
    if (valTok.empty())
        return current;

    if (inLoop)
        return refineByLoop(loadVar, valTok, addrTok, loadBB, fun, current, context);
    return refineByGuards(loadVar, valTok, addrTok, loadBB, fun, current, context);
}

Range RangeAnalysis::refineByGuards(const SVFVar* /*loadVar*/, const std::string& valTok,
                                    const std::string& addrTok,
                                    const SVFBasicBlock* loadBB, const FunObjVar* fun,
                                    const Range& current, const ICFGNode* context)
{
    auto git = guardIndex.find(valTok);
    if (git == guardIndex.end())
        return current;

    Range r = current;
    for (const GuardEntry& g : git->second)
    {
        if (g.fun != fun)
            continue;
        // The load executes only when the branch took the guarded successor.
        if (!fun->dominate(g.succBB, loadBB))
            continue;
        // Straight-line region only here (loops are handled separately, since
        // their back-edge stores would always trip the kill-check below).
        if (fun->hasLoopInfo(g.succBB))
            continue;

        // Kill-check (soundness): any store to the same slot inside the guarded
        // region (dominated by succBB) may change the value between the branch
        // and the load -> the predicate no longer transfers; skip narrowing.
        bool killed = false;
        auto sit = slotStores.find(addrTok);
        if (sit != slotStores.end())
            for (const StoreStmt* st : sit->second)
            {
                const ICFGNode* sn = st->getICFGNode();
                const SVFBasicBlock* sbb = sn ? sn->getBB() : nullptr;
                if (sbb && fun->dominate(g.succBB, sbb)) { killed = true; break; }
            }
        if (killed)
            continue;

        const Range bnd = analyzeVarRange(g.bndSide, context);
        const Range iv = evalGuardInterval(g.predicate, g.idxIsOp0, g.condTrue, bnd);
        if (iv.isTop())
            continue;
        const Range m = Range::meet(r, iv);
        if (!m.isBottom()) // never narrow to the empty set (stay conservative)
            r = m;
    }
    return r;
}

Range RangeAnalysis::refineByLoop(const SVFVar* /*loadVar*/, const std::string& valTok,
                                  const std::string& addrTok,
                                  const SVFBasicBlock* loadBB, const FunObjVar* fun,
                                  const Range& current, const ICFGNode* context)
{
    if (!fun->hasLoopInfo(loadBB))
        return current;
    const std::vector<const SVFBasicBlock*>& loop = fun->getLoopInfo(loadBB);
    if (loop.empty())
        return current;

    // 1. Loop guard: a branch comparing this slot whose guarded successor (the
    //    continue/enter edge) lies inside the loop and dominates the load.
    auto git = guardIndex.find(valTok);
    if (git == guardIndex.end())
        return current;
    const GuardEntry* loopGuard = nullptr;
    for (const GuardEntry& g : git->second)
    {
        if (g.fun != fun)
            continue;
        if (!fun->loopContainsBB(loop, g.succBB))
            continue;
        if (g.succBB != loadBB && !fun->dominate(g.succBB, loadBB))
            continue;
        loopGuard = &g;
        break;
    }
    if (!loopGuard)
        return current;

    auto sit = slotStores.find(addrTok);
    if (sit == slotStores.end())
        return current;
    const std::vector<const StoreStmt*>& stores = sit->second;

    // 2. Monotone step: a store inside the loop of (slot +/- positive const).
    int dir = 0; // +1 increasing, -1 decreasing
    for (const StoreStmt* st : stores)
    {
        const ICFGNode* sn = st->getICFGNode();
        const SVFBasicBlock* sbb = sn ? sn->getBB() : nullptr;
        if (!sbb || !fun->loopContainsBB(loop, sbb))
            continue;
        const SVFVar* rhs = st->getRHSVar();
        if (!rhs || !rhs->hasIncomingEdges(SVFStmt::PEDGEK::BinaryOp))
            continue;
        for (auto it = rhs->getIncomingEdgesBegin(SVFStmt::PEDGEK::BinaryOp);
             it != rhs->getIncomingEdgesEnd(SVFStmt::PEDGEK::BinaryOp); ++it)
        {
            const BinaryOPStmt* b = SVFUtil::dyn_cast<BinaryOPStmt>(*it);
            if (!b || b->getOpVarNum() != 2)
                continue;
            const u32_t oc = b->getOpcode();
            const bool add = (oc == BinaryOPStmt::Add);
            const bool sub = (oc == BinaryOPStmt::Sub);
            if (!add && !sub)
                continue;
            const SVFVar* a0 = b->getOpVar(0);
            const SVFVar* a1 = b->getOpVar(1);
            const bool m0 = (locationToken(a0) == valTok);
            const bool m1 = (locationToken(a1) == valTok);
            if (!m0 && !m1)
                continue;
            const SVFVar* other = m0 ? a1 : a0;
            const Range ov = analyzeVarRange(other, context);
            if (!ov.isConstant())
                continue;
            long long step = ov.getLower();
            if (sub)
                step = -step;
            if (step > 0) dir = 1;
            else if (step < 0) dir = -1;
            break;
        }
        if (dir)
            break;
    }
    if (dir == 0)
        return current;

    // 3. Init: a store to the slot OUTSIDE the loop that dominates the guard
    //    branch (the pre-header initialisation).
    Range init = Range::BOTTOM;
    for (const StoreStmt* st : stores)
    {
        const ICFGNode* sn = st->getICFGNode();
        const SVFBasicBlock* sbb = sn ? sn->getBB() : nullptr;
        if (!sbb || fun->loopContainsBB(loop, sbb))
            continue;
        if (!fun->dominate(sbb, loopGuard->branchBB))
            continue;
        init = Range::join(init, analyzeVarRange(st->getRHSVar(), context));
    }
    if (init.isBottom())
        return current; // no recognised pre-header init -> bail (stay sound)

    // 4. Compose [init, guard-bound] (increasing) or [guard-bound, init].
    const Range bnd = analyzeVarRange(loopGuard->bndSide, context);
    const Range gv = evalGuardInterval(loopGuard->predicate, loopGuard->idxIsOp0,
                                       loopGuard->condTrue, bnd);
    Range loopRange;
    if (dir > 0)
    {
        const Range::BoundType up = gv.isTop() ? Range::INF : gv.getUpper();
        loopRange = Range(init.getLower(), up);
    }
    else
    {
        const Range::BoundType lo = gv.isTop() ? Range::NINF : gv.getLower();
        loopRange = Range(lo, init.getUpper());
    }
    if (loopRange.isBottom())
        return current;
    const Range m = Range::meet(current, loopRange);
    return m.isBottom() ? current : m;
}

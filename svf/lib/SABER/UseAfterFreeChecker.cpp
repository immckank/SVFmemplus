// svf/lib/SABER/UseAfterFreeChecker.cpp
#include "SABER/UseAfterFreeChecker.h"
#include "SABER/SaberSliceExport.h"
#include "SABER/SaberCheckerAPI.h"
#include "SABER/SaberScopeAPI.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFVariables.h"
#include "Util/SVFUtil.h"
#include "Util/SVFStat.h"
#include "Util/Options.h"
#include "SABER/ProgSlice.h"
#include "Graphs/VFGNode.h"
#include "MemoryModel/PointsTo.h"
#include "MemoryModel/PointerAnalysis.h"
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace SVF;
using namespace SVFUtil;

static constexpr double kSlowSourceReportSec = 1.0;
static constexpr double kSlowPairCheckSec = 3.0;

const char* UseAfterFreeChecker::sliceExportGeneratedBy() const
{
    return "SVFmemplus-UseAfterFreeChecker";
}

void UseAfterFreeChecker::onPendingReportsFlushed(u32_t pendingCount, u32_t emittedCount)
{
    SVFUtil::outs() << "[UAF][flush-done] pending=" << pendingCount
                    << " emitted=" << emittedCount << "\n";
}

static bool pagVarHasGepInChain(const SVFVar* var, u32_t maxDepth = 8)
{
    if (var == nullptr)
        return false;

    std::unordered_set<NodeID> visited;
    std::deque<const SVFVar*> worklist;
    worklist.push_back(var);

    while (!worklist.empty() && visited.size() <= maxDepth)
    {
        const SVFVar* cur = worklist.front();
        worklist.pop_front();
        if (!visited.insert(cur->getId()).second)
            continue;

        if (SVFUtil::isa<GepValVar>(cur))
            return true;

        SVFVar* mut = const_cast<SVFVar*>(cur);
        for (const SVFStmt* edge : mut->getIncomingEdges(SVFStmt::Gep))
        {
            (void)edge;
            return true;
        }
        for (const SVFStmt* edge : mut->getIncomingEdges(SVFStmt::Copy))
            worklist.push_back(edge->getSrcNode());
        for (const SVFStmt* edge : mut->getIncomingEdges(SVFStmt::Load))
            worklist.push_back(edge->getSrcNode());
    }
    return false;
}

void UseAfterFreeChecker::setCurSlice(const SVFGNode* src)
{
    SrcSnkDDA::setCurSlice(src);
    if (enableSliceLocalUAFPairing())
        getCurSlice()->enableUAFNodeTracking();
}

void UseAfterFreeChecker::FWProcessCurNode(const DPIm& item)
{
    const SVFGNode* node = getNode(item.getCurNodeID());
    if (isSink(node))
    {
        addSinkToCurSlice(node);
        getCurSlice()->setPartialReachable();
        if (enableSliceLocalUAFPairing() && getCurSlice()->isUAFNodeTrackingEnabled())
        {
            if (freeNodes.find(node) != freeNodes.end())
                getCurSlice()->addToUAFFreeNodes(node);
            if (useNodes.find(node) != useNodes.end())
                getCurSlice()->addToUAFUseNodes(node);
        }
        if (freeAsSourceMode_ && node == getCurSlice()->getSource())
            addToCurForwardSlice(node);
    }
    else
    {
        addToCurForwardSlice(node);
    }
}

/*!
 * Whether an ICFG node lives in system/STL/generated code, so UAF should skip
 * frees/uses anchored there (e.g. <stop_token>, <bits/invoke.h>). Delegates to the
 * shared, queryable SaberScopeAPI table (dumpable via -saber-scope-dump). A raw
 * substring search is used because CallICFGNode::getSourceLoc() carries a node-type
 * prefix ("CallICFGNode: { ... }") that is not valid JSON.
 */
static bool uafIsSystemOrGeneratedCodeICFG(const ICFGNode* node)
{
    if (node == nullptr)
        return false;
    return SaberScopeAPI::getScopeAPI()->isOutOfScopePath(node->getSourceLoc());
}

/*!
 * Initialize sinks
 */
void UseAfterFreeChecker::initSnks()
{
    SVFIR* pag = getPAG();

    for(SVFIR::CSToArgsListMap::iterator it = pag->getCallSiteArgsMap().begin(),
            eit = pag->getCallSiteArgsMap().end(); it != eit; ++it)
    {
        CallGraph::FunctionSet callees;
        getCallgraph()->getCallees(it->first, callees);
        for(CallGraph::FunctionSet::const_iterator cit = callees.begin(), ecit = callees.end(); cit != ecit; ++cit)
        {
            const FunObjVar* fun = *cit;
            if(!SaberCheckerAPI::getCheckerAPI()->isMemDealloc(fun))
                continue;
            // Skip frees anchored in system/STL/generated code (noise-only pairs)
            if(uafIsSystemOrGeneratedCodeICFG(it->first))
                continue;
            SVFIR::SVFVarList &arglist = it->second;
            for (const PAGNode* pagNode : arglist)
            {
                if(!pagNode->isPointer())
                    continue;
                const SVFGNode* src = getSVFG()->getActualParmVFGNode(pagNode, it->first);
                if(src == nullptr)
                    continue;
                addToSinks(src);
                addToFreeNodes(src);
            }
        }
    }

    for (SVFIR::iterator it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        SVFVar* var = it->second;
        if(!var->isPointer())
            continue;

        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Load))
        {
            if(getSVFG()->hasStmtVFGNode(ld)){
                const SVFGNode* useNode = getSVFG()->getStmtVFGNode(ld);
                // Skip uses anchored in system/STL/generated code (noise-only pairs)
                if(uafIsSystemOrGeneratedCodeICFG(useNode->getICFGNode()))
                    continue;
                addToSinks(useNode);
                addToUseNodes(useNode);
            }
        }
        for(const SVFStmt* st : var->getIncomingEdges(SVFStmt::Store))
        {
            if(getSVFG()->hasStmtVFGNode(st)){
                const SVFGNode* useNode = getSVFG()->getStmtVFGNode(st);
                if(uafIsSystemOrGeneratedCodeICFG(useNode->getICFGNode()))
                    continue;
                addToSinks(useNode);
                addToUseNodes(useNode);
            }
        }
    }
}

static bool icfgReachable(const ICFGNode* start, const ICFGNode* target);
static bool isBackEdge(const ICFGEdge* edge);
static bool sameFunctionAcyclicReachable(const ICFGNode* start, const ICFGNode* target);
static bool sameFunctionFreeOrdersUse(const ICFGNode* freeICFG, const ICFGNode* useICFG);

static const SVFVar* getGepBaseValVar(const SVFVar* var)
{
    while (var != nullptr)
    {
        if (const GepValVar* gep = SVFUtil::dyn_cast<GepValVar>(var))
            var = gep->getBaseNode();
        else
            break;
    }
    return var;
}

static bool ptsMayAlias(PointerAnalysis* pta, const SVFVar* a, const SVFVar* b)
{
    if (a == nullptr || b == nullptr)
        return true;
    if (a->getId() == b->getId())
        return true;

    const PointsTo& apt = pta->getPts(a->getId());
    const PointsTo& bpt = pta->getPts(b->getId());
    if (apt.empty() || bpt.empty())
        return true;
    return apt.intersects(bpt);
}

static bool pointerVarsMayAlias(PointerAnalysis* pta, const SVFVar* a, const SVFVar* b)
{
    if (ptsMayAlias(pta, a, b))
        return true;

    const SVFVar* aBase = getGepBaseValVar(a);
    const SVFVar* bBase = getGepBaseValVar(b);
    if (aBase != a && ptsMayAlias(pta, aBase, b))
        return true;
    if (bBase != b && ptsMayAlias(pta, a, bBase))
        return true;
    if (aBase != a && bBase != b && ptsMayAlias(pta, aBase, bBase))
        return true;
    return false;
}

const SVFVar* UseAfterFreeChecker::getFreedPointerVar(const SVFGNode* freeNode)
{
    if (const ActualParmVFGNode* ap = SVFUtil::dyn_cast<ActualParmVFGNode>(freeNode))
        return SVFUtil::cast<SVFVar>(ap->getParam());
    return nullptr;
}

const SVFVar* UseAfterFreeChecker::getUsePointerVar(const SVFGNode* useNode)
{
    if (const LoadVFGNode* ld = SVFUtil::dyn_cast<LoadVFGNode>(useNode))
    {
        const SVFVar* dst = ld->getPAGDstNode();
        const SVFVar* src = ld->getPAGSrcNode();
        if (dst != nullptr && dst->isPointer())
            return dst;
        return src;
    }
    if (const StoreVFGNode* st = SVFUtil::dyn_cast<StoreVFGNode>(useNode))
        return st->getPAGDstNode();
    return nullptr;
}

static const SVFVar* getUseDerefPointerVar(const SVFGNode* useNode)
{
    if (const LoadVFGNode* ld = SVFUtil::dyn_cast<LoadVFGNode>(useNode))
        return ld->getPAGSrcNode();
    if (const StoreVFGNode* st = SVFUtil::dyn_cast<StoreVFGNode>(useNode))
        return st->getPAGDstNode();
    return nullptr;
}

static bool collectAliasPtsForVar(PointerAnalysis* pta, const SVFVar* var, PointsTo& pts)
{
    if (pta == nullptr || var == nullptr)
        return false;

    bool hasPrecisePts = false;
    const PointsTo& directPts = pta->getPts(var->getId());
    if (!directPts.empty())
    {
        pts |= directPts;
        hasPrecisePts = true;
    }

    const SVFVar* base = getGepBaseValVar(var);
    if (base != nullptr && base != var)
    {
        const PointsTo& basePts = pta->getPts(base->getId());
        if (!basePts.empty())
        {
            pts |= basePts;
            hasPrecisePts = true;
        }
    }

    return hasPrecisePts;
}

static bool collectAliasPtsForUseNode(PointerAnalysis* pta, const SVFGNode* useNode, PointsTo& pts)
{
    if (useNode == nullptr)
        return false;

    bool hasPrecisePts = false;
    if (const LoadVFGNode* ld = SVFUtil::dyn_cast<LoadVFGNode>(useNode))
    {
        hasPrecisePts |= collectAliasPtsForVar(pta, ld->getPAGSrcNode(), pts);
        const SVFVar* dst = ld->getPAGDstNode();
        if (dst != nullptr && dst->isPointer())
            hasPrecisePts |= collectAliasPtsForVar(pta, dst, pts);
        return hasPrecisePts;
    }
    if (const StoreVFGNode* st = SVFUtil::dyn_cast<StoreVFGNode>(useNode))
        return collectAliasPtsForVar(pta, st->getPAGDstNode(), pts);

    return collectAliasPtsForVar(pta, getUseDerefPointerVar(useNode), pts);
}

static const ArgValVar* getBaseArgValVar(const SVFVar* var)
{
    const SVFVar* base = getGepBaseValVar(var);
    return base == nullptr ? nullptr : SVFUtil::dyn_cast<ArgValVar>(base);
}

static bool actualArgMatchesFunctionArg(PointerAnalysis* pta,
                                        const SVFVar* actualArg,
                                        const SVFVar* calleeVar,
                                        u32_t actualArgIndex,
                                        const FunObjVar* calleeFun)
{
    if (actualArg == nullptr || calleeVar == nullptr)
        return false;

    if (pointerVarsMayAlias(pta, actualArg, calleeVar))
        return true;

    const ArgValVar* formalArg = getBaseArgValVar(calleeVar);
    return formalArg != nullptr && formalArg->getFunction() == calleeFun &&
           formalArg->getArgNo() == actualArgIndex;
}

static bool actualArgMatchesUse(PointerAnalysis* pta,
                                const SVFVar* actualArg,
                                const SVFGNode* useNode,
                                u32_t actualArgIndex,
                                const FunObjVar* useFun)
{
    if (actualArg == nullptr || useNode == nullptr)
        return false;

    const SVFVar* derefPtr = getUseDerefPointerVar(useNode);
    if (actualArgMatchesFunctionArg(pta, actualArg, derefPtr, actualArgIndex, useFun))
        return true;

    if (const LoadVFGNode* ld = SVFUtil::dyn_cast<LoadVFGNode>(useNode))
    {
        const SVFVar* dst = ld->getPAGDstNode();
        if (dst != nullptr && dst->isPointer() &&
                actualArgMatchesFunctionArg(pta, actualArg, dst, actualArgIndex, useFun))
            return true;
    }

    return false;
}

struct UAFUsePtsIndex
{
    std::unordered_map<NodeID, std::vector<const SVFGNode*>> usesByObject;
    std::vector<const SVFGNode*> impreciseUses;
};

struct UAFIndexedCallArg
{
    const SVFVar* var;
    u32_t index;
};

struct UAFDirectCallSiteInfo
{
    const CallICFGNode* cs;
    std::vector<UAFIndexedCallArg> pointerArgs;
};

struct UAFCallPairHash
{
    size_t operator()(const std::pair<const FunObjVar*, const FunObjVar*>& p) const
    {
        return (reinterpret_cast<size_t>(p.first) >> 4) ^
               (reinterpret_cast<size_t>(p.second) << 3);
    }
};

struct UAFDirectCallIndex
{
    const SVFIR* pag = nullptr;
    std::unordered_map<std::pair<const FunObjVar*, const FunObjVar*>,
                       std::vector<UAFDirectCallSiteInfo>, UAFCallPairHash> calls;
};

static const std::vector<UAFDirectCallSiteInfo>* lookupDirectCalls(SVFIR* pag,
        CallGraph* callgraph,
        const FunObjVar* caller,
        const FunObjVar* callee)
{
    static UAFDirectCallIndex index;
    if (pag == nullptr || callgraph == nullptr || caller == nullptr || callee == nullptr)
        return nullptr;

    if (index.pag != pag)
    {
        index.pag = pag;
        index.calls.clear();
        for (SVFIR::CSToArgsListMap::iterator it = pag->getCallSiteArgsMap().begin(),
                eit = pag->getCallSiteArgsMap().end(); it != eit; ++it)
        {
            const CallICFGNode* cs = it->first;
            if (cs == nullptr || cs->getFun() == nullptr)
                continue;

            UAFDirectCallSiteInfo info;
            info.cs = cs;
            u32_t argIndex = 0;
            for (const PAGNode* pagNode : it->second)
            {
                if (pagNode->isPointer())
                    info.pointerArgs.push_back(
                        {SVFUtil::cast<SVFVar>(pagNode), argIndex});
                ++argIndex;
            }

            CallGraph::FunctionSet callees;
            callgraph->getCallees(cs, callees);
            for (const FunObjVar* dstFun : callees)
                index.calls[std::make_pair(cs->getFun(), dstFun)].push_back(info);
        }
    }

    auto found = index.calls.find(std::make_pair(caller, callee));
    return found == index.calls.end() ? nullptr : &found->second;
}

static void buildUAFUsePtsIndex(const SrcSnkDDA::SVFGNodeSet& useSet, PointerAnalysis* pta,
                                UAFUsePtsIndex& index)
{
    index.usesByObject.clear();
    index.impreciseUses.clear();
    index.usesByObject.reserve(useSet.size());

    for (auto uit = useSet.begin(), euit = useSet.end(); uit != euit; ++uit)
    {
        const SVFGNode* useNode = *uit;
        PointsTo usePts;
        if (pta == nullptr || !collectAliasPtsForUseNode(pta, useNode, usePts) || usePts.empty())
        {
            index.impreciseUses.push_back(useNode);
            continue;
        }

        for (PointsTo::iterator pit = usePts.begin(), epit = usePts.end(); pit != epit; ++pit)
            index.usesByObject[*pit].push_back(useNode);
    }
}

static void collectObjectCandidateUsesFromIndex(const UAFUsePtsIndex& index,
                                                PointerAnalysis* pta,
                                                const SVFVar* freed,
                                                const SrcSnkDDA::SVFGNodeSet& fallbackUseSet,
                                                std::vector<const SVFGNode*>& outUses)
{
    outUses.clear();
    if (pta == nullptr || freed == nullptr)
    {
        outUses.insert(outUses.end(), fallbackUseSet.begin(), fallbackUseSet.end());
        return;
    }

    PointsTo freePts;
    if (!collectAliasPtsForVar(pta, freed, freePts) || freePts.empty())
    {
        outUses.insert(outUses.end(), fallbackUseSet.begin(), fallbackUseSet.end());
        return;
    }

    std::unordered_set<NodeID> inserted;
    for (PointsTo::iterator pit = freePts.begin(), epit = freePts.end(); pit != epit; ++pit)
    {
        auto it = index.usesByObject.find(*pit);
        if (it == index.usesByObject.end())
            continue;
        for (const SVFGNode* useNode : it->second)
        {
            if (useNode != nullptr && inserted.insert(useNode->getId()).second)
                outUses.push_back(useNode);
        }
    }

    for (const SVFGNode* useNode : index.impreciseUses)
    {
        if (useNode != nullptr && inserted.insert(useNode->getId()).second)
            outUses.push_back(useNode);
    }
}

static bool mrNodeIntersectsPts(const SVFGNode* node, const PointsTo& pts)
{
    if (node == nullptr || pts.empty())
        return false;

    const MRSVFGNode* mrNode = SVFUtil::dyn_cast<MRSVFGNode>(node);
    if (mrNode == nullptr)
        return false;

    PointsTo mrPts;
    mrPts |= mrNode->getPointsTo();
    return !mrPts.empty() && mrPts.intersects(pts);
}

static bool shouldPruneReturnWithoutFreedMemoryOut(const SVFGEdge* edge,
                                                   const PointsTo& freedPts)
{
    if (edge == nullptr || !edge->isRetIndirectVFGEdge() || freedPts.empty())
        return false;

    return !mrNodeIntersectsPts(edge->getSrcNode(), freedPts) &&
           !mrNodeIntersectsPts(edge->getDstNode(), freedPts);
}

static void collectReturnPrunedValueFlowUses(
    const SVFGNode* freeNode,
    const SrcSnkDDA::SVFGNodeSet& useNodes,
    const PointsTo& freedPts,
    u32_t backwardBudget,
    u32_t forwardBudget,
    std::vector<const SVFGNode*>& outUses)
{
    outUses.clear();
    if (freeNode == nullptr)
        return;

    std::deque<const SVFGNode*> worklist;
    std::unordered_set<NodeID> backwardVisited;
    std::vector<const SVFGNode*> backwardNodes;

    worklist.push_back(freeNode);
    while (!worklist.empty() && backwardVisited.size() < backwardBudget)
    {
        const SVFGNode* node = worklist.front();
        worklist.pop_front();
        if (node == nullptr || !backwardVisited.insert(node->getId()).second)
            continue;
        backwardNodes.push_back(node);

        for (auto edge : node->getInEdges())
        {
            if (shouldPruneReturnWithoutFreedMemoryOut(edge, freedPts))
                continue;
            worklist.push_back(edge->getSrcNode());
        }
    }

    std::unordered_set<NodeID> forwardVisited;
    std::unordered_set<NodeID> insertedUses;
    for (const SVFGNode* seed : backwardNodes)
        worklist.push_back(seed);

    while (!worklist.empty() && forwardVisited.size() < forwardBudget)
    {
        const SVFGNode* node = worklist.front();
        worklist.pop_front();
        if (node == nullptr || !forwardVisited.insert(node->getId()).second)
            continue;

        if (node != freeNode && backwardVisited.find(node->getId()) == backwardVisited.end() &&
                useNodes.find(node) != useNodes.end() &&
                insertedUses.insert(node->getId()).second)
            outUses.push_back(node);

        for (auto edge : node->getOutEdges())
        {
            if (shouldPruneReturnWithoutFreedMemoryOut(edge, freedPts))
                continue;
            worklist.push_back(edge->getDstNode());
        }
    }
}

template <typename UseRange>
static void collectObjectCandidateUsesImpl(const UseRange& candidateUseSet,
                                           PointerAnalysis* pta,
                                           const SVFVar* freed,
                                           std::vector<const SVFGNode*>& outUses)
{
    outUses.clear();
    if (pta == nullptr || freed == nullptr)
    {
        outUses.insert(outUses.end(), candidateUseSet.begin(), candidateUseSet.end());
        return;
    }

    PointsTo freePts;
    if (!collectAliasPtsForVar(pta, freed, freePts) || freePts.empty())
    {
        outUses.insert(outUses.end(), candidateUseSet.begin(), candidateUseSet.end());
        return;
    }

    std::unordered_set<NodeID> inserted;
    for (const SVFGNode* useNode : candidateUseSet)
    {
        PointsTo usePts;
        if (!collectAliasPtsForUseNode(pta, useNode, usePts) || usePts.empty())
        {
            if (useNode != nullptr && inserted.insert(useNode->getId()).second)
                outUses.push_back(useNode);
            continue;
        }

        if (freePts.intersects(usePts) &&
                useNode != nullptr && inserted.insert(useNode->getId()).second)
            outUses.push_back(useNode);
    }
}

void UseAfterFreeChecker::collectObjectCandidateUses(const SVFGNodeSet& candidateUseSet,
                                                     const SVFGNode* freeNode,
                                                     std::vector<const SVFGNode*>& outUses) const
{
    const SVFVar* freed = getFreedPointerVar(freeNode);
    PointerAnalysis* pta = getSVFG() == nullptr ? nullptr : getSVFG()->getPTA();
    collectObjectCandidateUsesImpl(candidateUseSet, pta, freed, outUses);
}

void UseAfterFreeChecker::collectObjectCandidateUses(
    const std::vector<const SVFGNode*>& candidateUseSet,
    const SVFGNode* freeNode,
    std::vector<const SVFGNode*>& outUses) const
{
    const SVFVar* freed = getFreedPointerVar(freeNode);
    PointerAnalysis* pta = getSVFG() == nullptr ? nullptr : getSVFG()->getPTA();
    collectObjectCandidateUsesImpl(candidateUseSet, pta, freed, outUses);
}

bool UseAfterFreeChecker::maySameFreedObject(const SVFGNode* freeNode,
                               const SVFGNode* useNode) const
{
    const SVFVar* freed = getFreedPointerVar(freeNode);
    const SVFVar* used = getUsePointerVar(useNode);
    if (freed == nullptr || used == nullptr)
        return true;

    const SVFG* svfg = getSVFG();
    PointerAnalysis* pta = svfg->getPTA();
    if (pta == nullptr)
        return true;

    if (pointerVarsMayAlias(pta, freed, used))
        return true;

    if (const LoadVFGNode* ld = SVFUtil::dyn_cast<LoadVFGNode>(useNode))
    {
        const SVFVar* dst = ld->getPAGDstNode();
        if (dst != nullptr && dst->isPointer() && pointerVarsMayAlias(pta, freed, dst))
            return true;
    }

    const ICFGNode* freeICFG = freeNode->getICFGNode();
    const ICFGNode* useICFG = useNode->getICFGNode();
    if (freeICFG == nullptr || useICFG == nullptr)
        return false;

    const FunObjVar* freeFun = freeICFG->getFun();
    const FunObjVar* useFun = useICFG->getFun();
    if (freeFun == nullptr || useFun == nullptr || freeFun == useFun)
        return false;

    const SVFVar* useBase = getGepBaseValVar(used);
    if (useBase == nullptr)
        useBase = used;

    SVFIR* pag = getPAG();
    if (const std::vector<UAFDirectCallSiteInfo>* callsites =
                lookupDirectCalls(pag, getCallgraph(), useFun, freeFun))
    {
        for (const UAFDirectCallSiteInfo& info : *callsites)
        {
            for (const UAFIndexedCallArg& arg : info.pointerArgs)
            {
                if (actualArgMatchesFunctionArg(pta, arg.var, freed, arg.index, freeFun) &&
                        pointerVarsMayAlias(pta, arg.var, useBase))
                    return true;
            }
        }
    }

    if (const std::vector<UAFDirectCallSiteInfo>* callsites =
                lookupDirectCalls(pag, getCallgraph(), freeFun, useFun))
    {
        for (const UAFDirectCallSiteInfo& info : *callsites)
        {
            for (const UAFIndexedCallArg& arg : info.pointerArgs)
            {
                if (pointerVarsMayAlias(pta, arg.var, freed) &&
                        actualArgMatchesUse(pta, arg.var, useNode, arg.index, useFun))
                    return true;
            }
        }
    }

    return false;
}

ProgSlice::Condition UseAfterFreeChecker::computeControlOrderGuard(ProgSlice* slice,
        const SVFGNode* freeNode, const SVFGNode* useNode)
{
    const ICFGNode* freeICFG = freeNode->getICFGNode();
    const ICFGNode* useICFG = useNode->getICFGNode();
    if (freeICFG == nullptr || useICFG == nullptr)
        return slice->getFalseCond();
    if (freeICFG == useICFG)
        return slice->getFalseCond();

    const SVFBasicBlock* freeBB = slice->getSVFGNodeBB(freeNode);
    const SVFBasicBlock* useBB = slice->getSVFGNodeBB(useNode);
    const FunObjVar* freeFun = freeICFG->getFun();
    const FunObjVar* useFun = useICFG->getFun();

    if (freeFun != nullptr && freeFun == useFun && freeBB != nullptr && useBB != nullptr)
    {
        if (freeBB == useBB)
        {
            if (icfgReachable(freeICFG, useICFG))
                return slice->getTrueCond();
            return slice->getFalseCond();
        }
        return slice->ComputeIntraVFGGuard(freeBB, useBB);
    }

    if (icfgReachable(freeICFG, useICFG))
        return slice->getTrueCond();
    return slice->getFalseCond();
}

bool UseAfterFreeChecker::isFeasibleFreeUsePair(ProgSlice* slice,
                                  const SVFGNode* freeNode, const SVFGNode* useNode,
                                  ProgSlice::Condition& outGuard) const
{
    if (!maySameFreedObject(freeNode, useNode))
        return false;
    if (shouldSkipCrossFileAllocNoise(freeNode, useNode))
        return false;

    ProgSlice::Condition orderCond = computeControlOrderGuard(slice, freeNode, useNode);
    if (slice->isEquivalentBranchCond(orderCond, slice->getFalseCond()))
        return false;

    outGuard = slice->condAnd(slice->getVFCond(freeNode), slice->getVFCond(useNode));
    outGuard = slice->condAnd(outGuard, orderCond);
    return !slice->isEquivalentBranchCond(outGuard, slice->getFalseCond());
}

static bool icfgReachable(const ICFGNode* start, const ICFGNode* target) {
    std::unordered_set<const ICFGNode*> visited;
    std::stack<const ICFGNode*> worklist;
    worklist.push(start);

    while (!worklist.empty()) {
        const ICFGNode* cur = worklist.top();
        worklist.pop();

        if (cur == target)
            return true;

        if (!visited.insert(cur).second)
            continue; // already visited

        for (auto edge : cur->getOutEdges()) {
            ICFGNode* succ = edge->getDstNode();
            worklist.push(succ);
        }
    }
    return false;
}

static bool sameFunctionAcyclicReachable(const ICFGNode* start, const ICFGNode* target)
{
    if (start == nullptr || target == nullptr)
        return false;

    const FunObjVar* fun = start->getFun();
    if (fun == nullptr || fun != target->getFun())
        return false;

    std::unordered_set<const ICFGNode*> visited;
    std::deque<const ICFGNode*> worklist;
    worklist.push_back(start);

    const size_t maxVisitedNodes = Options::SaberUAFReachMaxNodes();
    while (!worklist.empty())
    {
        const ICFGNode* node = worklist.front();
        worklist.pop_front();

        if (node == target)
            return true;
        if (!visited.insert(node).second)
            continue;
        if (visited.size() >= maxVisitedNodes)
            return false;

        if (const CallICFGNode* call = SVFUtil::dyn_cast<CallICFGNode>(node))
        {
            const ICFGNode* ret = call->getRetICFGNode();
            if (ret != nullptr && ret->getFun() == fun)
                worklist.push_back(ret);
        }

        for (const ICFGEdge* edge : node->getOutEdges())
        {
            const ICFGNode* succ = edge->getDstNode();
            if (succ == nullptr || succ->getFun() != fun || isBackEdge(edge))
                continue;
            worklist.push_back(succ);
        }
    }

    return false;
}

static bool sameFunctionFreeOrdersUse(const ICFGNode* freeICFG, const ICFGNode* useICFG)
{
    if (freeICFG == nullptr || useICFG == nullptr || freeICFG == useICFG)
        return false;

    const FunObjVar* fun = freeICFG->getFun();
    if (fun == nullptr || fun != useICFG->getFun())
        return false;

    const SVFBasicBlock* freeBB = freeICFG->getBB();
    const SVFBasicBlock* useBB = useICFG->getBB();
    if (freeBB == nullptr || useBB == nullptr)
        return sameFunctionAcyclicReachable(freeICFG, useICFG);

    if (freeBB == useBB)
        return sameFunctionAcyclicReachable(freeICFG, useICFG);

    return fun->getLoopAndDomInfo() != nullptr &&
           fun->getLoopAndDomInfo()->dominate(freeBB, useBB) &&
           sameFunctionAcyclicReachable(freeICFG, useICFG);
}

// Redefinition ("free-then-reassign") kill. The freed pointer value is loaded from an
// underlying variable slot `pAddr` (e.g. a local `p`). If `pAddr` is reassigned -- the
// canonical `kfree(p); p = NULL;` defensive idiom, or `p = other;` -- on a store that the
// free reaches and that dominates the use, then the use reads the NEW value, not the freed
// object, so it is not a use-after-free. This is the dominant FP shape on kernel cleanup
// code, where one free is followed by many later accesses after the pointer was cleared.
// Conservative by construction (requires free->store->use dominance), so it never
// suppresses a genuine use that precedes the reassignment (e.g. `kfree(p); p->f` before any
// `p = NULL`).
static bool freedPointerReassignedBeforeUse(const SVFVar* freedVal,
                                            const ICFGNode* freeICFG,
                                            const ICFGNode* useICFG)
{
    if (freedVal == nullptr || freeICFG == nullptr || useICFG == nullptr)
        return false;
    if (freeICFG->getFun() == nullptr || freeICFG->getFun() != useICFG->getFun())
        return false;

    SVFVar* mutFreed = const_cast<SVFVar*>(freedVal);
    for (const SVFStmt* ldStmt : mutFreed->getIncomingEdges(SVFStmt::Load))
    {
        const LoadStmt* load = SVFUtil::dyn_cast<LoadStmt>(ldStmt);
        if (load == nullptr)
            continue;
        SVFVar* pAddr = load->getRHSVar();
        if (pAddr == nullptr)
            continue;
        for (const SVFStmt* stStmt : pAddr->getIncomingEdges(SVFStmt::Store))
        {
            const ICFGNode* storeICFG = stStmt->getICFGNode();
            if (storeICFG == nullptr || storeICFG == freeICFG)
                continue;
            if (sameFunctionFreeOrdersUse(freeICFG, storeICFG) &&
                    sameFunctionFreeOrdersUse(storeICFG, useICFG))
                return true;
        }
    }
    return false;
}

static bool getICFGSourceFileLine(const ICFGNode* node, std::string& file, u32_t& line)
{
    file.clear();
    line = 0;
    if (node == nullptr)
        return false;
    const std::string& loc = node->getSourceLoc();
    const size_t brace = loc.find('{');
    const std::string json = (brace == std::string::npos) ? loc : loc.substr(brace);
    SaberSliceExportUtil::parseLocFileLine(json, file, line);
    return !file.empty() && line != 0;
}

static void collectReachableUsesFromFree(
    const ICFGNode* start,
    const std::unordered_map<const ICFGNode*, std::vector<const SVFGNode*>>& usesByICFGNode,
    const size_t totalIndexedUses,
    const size_t maxVisitedNodes,
    std::vector<const SVFGNode*>& outUses)
{
    if (start == nullptr || usesByICFGNode.empty() || totalIndexedUses == 0)
        return;

    std::unordered_set<const ICFGNode*> visited;
    std::deque<const ICFGNode*> worklist;
    worklist.push_back(start);

    size_t visitedNodes = 0;
    while (!worklist.empty())
    {
        const ICFGNode* node = worklist.front();
        worklist.pop_front();

        if (!visited.insert(node).second)
            continue;
        ++visitedNodes;
        if (visitedNodes >= maxVisitedNodes)
            return;

        auto it = usesByICFGNode.find(node);
        if (it != usesByICFGNode.end())
        {
            outUses.insert(outUses.end(), it->second.begin(), it->second.end());
            if (outUses.size() >= totalIndexedUses)
                return;
        }

        for (const ICFGEdge* edge : node->getOutEdges())
        {
            worklist.push_back(edge->getDstNode());
        }
    }
}

static bool isBackEdge(const ICFGEdge* edge) {
    auto src = edge->getSrcNode();
    auto dst = edge->getDstNode();

    const FunObjVar* Fsrc = src->getFun();
    const FunObjVar* Fdst = dst->getFun();

    // 仅在同一函数内的边才可能是循环回边
    if (Fsrc != Fdst || !Fsrc) return false;

    const SVFBasicBlock* BBsrc = src->getBB();
    const SVFBasicBlock* BBdst = dst->getBB();
    if (!BBsrc || !BBdst) return false;

    //如果二者来自同一基本块，那么不会是回边，但同一基本块本身肯定支配自己，对这一情况予以排除
    if (BBsrc == BBdst) return false; 

    return Fsrc->getLoopAndDomInfo()->strictlyDominate(BBdst, BBsrc);
}


bool hasLoopBackEdge(const ICFGNode* src, const ICFGNode* dst) {
    std::unordered_set<const ICFGNode*> visited;
    std::deque<const ICFGNode*> worklist;
    worklist.push_back(src);

    while (!worklist.empty()) {
        const ICFGNode* node = worklist.front();
        worklist.pop_front();

        if (!visited.insert(node).second) continue;
        if (node == dst) return false;  // 找到了 use，不是循环引起的路径阻塞

        for (const ICFGEdge* e : node->getOutEdges()) {
            if (isBackEdge(e)) {
                // 只有当当前 src 能到达 dst，且回边位于同一可达区域中时才返回
                if (visited.count(e->getDstNode()) == 0){
                    return true;
                }
            }
            worklist.push_back(e->getDstNode());
        }
    }
    return false;
}


void UseAfterFreeChecker::runSliceFromSource(const SVFGNode* source, bool freeAsSource)
{
    freeAsSourceMode_ = freeAsSource;
    setCurSlice(source);

    if (freeAsSource)
    {
        if (const CallICFGNode* freeCS = getFreeCallICFGNode(source))
            addSrcToCSID(source, freeCS);
    }

    ContextCond cxt;
    DPIm item(source->getId(), cxt);

    const bool timeStat = Options::SaberTimeStat();
    double fwdStart = 0;
    if (timeStat)
        fwdStart = SVFStat::getClk(true);
    forwardTraverse(item);
    if (timeStat)
    {
        saberTimeStat.forwardTraverseTime += (SVFStat::getClk(true) - fwdStart) / TIMEINTERVAL;
        saberTimeStat.numSinks += getCurSlice()->getSinks().size();
        if (getCurSlice()->getForwardSliceSize() > saberTimeStat.uafMaxForwardSlice)
            saberTimeStat.uafMaxForwardSlice = getCurSlice()->getForwardSliceSize();
    }

    if (!(enableReachGlobalPrune() && getCurSlice()->isReachGlobal()))
    {
        double bwStart = 0;
        if (timeStat)
            bwStart = SVFStat::getClk(true);
        for (SVFGNodeSetIter sit = getCurSlice()->sinksBegin(), esit =
                    getCurSlice()->sinksEnd(); sit != esit; ++sit)
        {
            ContextCond bwCxt;
            DPIm bwItem((*sit)->getId(), bwCxt);
            backwardTraverse(bwItem);
        }
        if (timeStat)
            saberTimeStat.backwardTraverseTime += (SVFStat::getClk(true) - bwStart) / TIMEINTERVAL;

        if (needDefaultAllPathSolve())
        {
            double solveStart = 0;
            if (timeStat)
                solveStart = SVFStat::getClk(true);
            if (getCurSlice()->AllPathReachableSolve())
                getCurSlice()->setAllReachable();
            if (timeStat)
                saberTimeStat.solveTime += (SVFStat::getClk(true) - solveStart) / TIMEINTERVAL;
        }
    }

    if (getCurSlice()->getSinks().empty())
    {
        ++saberTimeStat.uafSourcesNoSinks;
    }
    else
    {
        ++saberTimeStat.uafSourcesWithSinks;
        if (getCurSlice()->getUAFFreeNodes().empty())
            ++saberTimeStat.uafSourcesNoFreeInSlice;
        if (getCurSlice()->getUAFUseNodes().empty())
            ++saberTimeStat.uafSourcesNoUseInSlice;
    }

    reportBug(getCurSlice());
    freeAsSourceMode_ = false;
}

void UseAfterFreeChecker::analyze()
{
    const bool progress = Options::UAFCheck();
    const bool timeStat = Options::SaberTimeStat();
    double totalStart = 0;
    if (progress || timeStat)
    {
        totalStart = SVFStat::getClk(true);
    }

    initialize();

    clearPendingReports();

    sliceCollector_ = SaberSliceCollector();
    prepareSliceCollector();

    if (timeStat)
    {
        saberTimeStat.uafNumFreeNodes = freeNodes.size();
        saberTimeStat.uafNumUseNodes = useNodes.size();
    }

    ContextCond::setMaxCxtLen(Options::CxtLimit());

    const u32_t numSrcs = getSources().size();
    if (progress || timeStat)
    {
        outs() << "[UAF][analyze-begin] sources=" << numSrcs
               << " freeNodes=" << freeNodes.size()
               << " useNodes=" << useNodes.size()
               << " allSinks=" << getSinks().size() << "\n";
        outs().flush();
    }

    analyzeFreeAnchoredUAFPairs();

    if (progress || timeStat)
    {
        const double elapsed = (SVFStat::getClk(true) - totalStart) / TIMEINTERVAL;
        if (timeStat)
            saberTimeStat.totalTime = elapsed;
        if (progress)
        {
            outs() << "[UAF][analyze-done] sources=" << numSrcs
                   << " withSinks=" << saberTimeStat.uafSourcesWithSinks
                   << " pending=" << pendingReports_.size()
                   << " reported=" << saberTimeStat.uafReportedSources
                   << " elapsed=" << elapsed << "\n";
            outs().flush();
        }
    }
    finalize();
}

bool UseAfterFreeChecker::isSatisfiableForFreeAndUsePairs(ProgSlice* slice, GenericBug::EventStack& eventStack){
    const bool timeStat = Options::SaberTimeStat();
    double checkStart = 0;
    if (timeStat)
        checkStart = SVFStat::getClk(true);

    bool flag = true;
    const SVFGNodeSet* freeSet = &freeNodes;
    const SVFGNodeSet* useSet = &useNodes;
    if (enableSliceLocalUAFPairing() && slice->isUAFNodeTrackingEnabled())
    {
        freeSet = &slice->getUAFFreeNodes();
        useSet = &slice->getUAFUseNodes();
    }

    u32_t pairChecks = 0;
    u32_t feasiblePairs = 0;
    const bool useBBGuardPair = Options::SaberUAFPairBBGuard();

    if (useBBGuardPair)
    {
        UAFUsePtsIndex useIndex;
        PointerAnalysis* pta = getSVFG() == nullptr ? nullptr : getSVFG()->getPTA();
        buildUAFUsePtsIndex(*useSet, pta, useIndex);

        for (SVFGNodeSetIter fit = freeSet->begin(), efit = freeSet->end(); fit != efit; ++fit)
        {
            const SVFGNode* freeNode = *fit;
            std::vector<const SVFGNode*> candidateUses;
            collectObjectCandidateUsesFromIndex(useIndex, pta, getFreedPointerVar(freeNode),
                                                *useSet, candidateUses);
            for (const SVFGNode* useNode : candidateUses)
            {
                ++pairChecks;
                ProgSlice::Condition guard;
                if (!isFeasibleFreeUsePair(slice, freeNode, useNode, guard))
                    continue;
                if (isDuplicateUAFPair(freeNode, useNode))
                    continue;

                const ICFGNode* ficfg = freeNode->getICFGNode();
                const ICFGNode* uicfg = useNode->getICFGNode();
                if (ficfg == nullptr || uicfg == nullptr)
                    continue;

                eventStack.push_back(SVFBugEvent(SVFBugEvent::Free, ficfg));
                slice->setFinalCond(slice->getVFCond(freeNode));
                slice->evalFinalCond2Event(eventStack);
                eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, uicfg));
                slice->setFinalCond(slice->getVFCond(useNode));
                slice->evalFinalCond2Event(eventStack);
                flag = false;
                ++feasiblePairs;
                markUAFPairReported(freeNode, useNode);

                if (Options::UAFLoopHint() && hasLoopBackEdge(ficfg, uicfg))
                    eventStack.push_back(SVFBugEvent(SVFBugEvent::PotentialLoop, ficfg));
            }
        }
    }
    else
    {
        std::unordered_map<const ICFGNode*, std::vector<const SVFGNode*>> usesByICFGNode;
        size_t totalIndexedUses = 0;
        usesByICFGNode.reserve(useSet->size());
        for (SVFGNodeSetIter uit = useSet->begin(), euit = useSet->end(); uit != euit; ++uit)
        {
            const ICFGNode* uicfg = (*uit)->getICFGNode();
            if (uicfg == nullptr)
                continue;
            usesByICFGNode[uicfg].push_back(*uit);
            ++totalIndexedUses;
        }

        for (SVFGNodeSetIter fit = freeSet->begin(), efit = freeSet->end(); fit != efit; ++fit)
        {
            const ICFGNode* ficfg = (*fit)->getICFGNode();
            if (ficfg == nullptr)
                continue;

            std::vector<const SVFGNode*> reachableUses;
            const size_t maxVisitedNodes = Options::SaberUAFReachMaxNodes();
            collectReachableUsesFromFree(ficfg, usesByICFGNode, totalIndexedUses, maxVisitedNodes, reachableUses);

            std::vector<const SVFGNode*> candidateUses;
            collectObjectCandidateUses(reachableUses, *fit, candidateUses);
            for (const SVFGNode* useNode : candidateUses)
            {
                ++pairChecks;
                if (isDuplicateUAFPair(*fit, useNode))
                    continue;
                if (!maySameFreedObject(*fit, useNode))
                    continue;
                if (shouldSkipCrossFileAllocNoise(*fit, useNode))
                    continue;

                ProgSlice::Condition orderCond = computeControlOrderGuard(slice, *fit, useNode);
                if (slice->isEquivalentBranchCond(orderCond, slice->getFalseCond()))
                    continue;

                ProgSlice::Condition guard = slice->condAnd(slice->getVFCond(*fit), slice->getVFCond(useNode));
                guard = slice->condAnd(guard, orderCond);
                if (!slice->isEquivalentBranchCond(guard, slice->getFalseCond()))
                {
                    const ICFGNode* uicfg = useNode->getICFGNode();
                    if (uicfg == nullptr)
                        continue;

                    eventStack.push_back(SVFBugEvent(SVFBugEvent::Free, (*fit)->getICFGNode()));
                    slice->setFinalCond(slice->getVFCond(*fit));
                    slice->evalFinalCond2Event(eventStack);
                    eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, uicfg));
                    slice->setFinalCond(slice->getVFCond(useNode));
                    slice->evalFinalCond2Event(eventStack);
                    flag = false;
                    ++feasiblePairs;
                    markUAFPairReported(*fit, useNode);

                    if (Options::UAFLoopHint() && hasLoopBackEdge(ficfg, uicfg))
                        eventStack.push_back(SVFBugEvent(SVFBugEvent::PotentialLoop, ficfg));
                }
            }
        }
    }

    if (timeStat)
    {
        double t = (SVFStat::getClk(true) - checkStart) / TIMEINTERVAL;
        saberTimeStat.uafPairCheckTime += t;
        saberTimeStat.uafTotalPairChecks += pairChecks;
        if (pairChecks > saberTimeStat.uafMaxPairChecks)
            saberTimeStat.uafMaxPairChecks = pairChecks;
        if (freeSet->size() > saberTimeStat.uafMaxSliceFreeNodes)
            saberTimeStat.uafMaxSliceFreeNodes = freeSet->size();
        if (useSet->size() > saberTimeStat.uafMaxSliceUseNodes)
            saberTimeStat.uafMaxSliceUseNodes = useSet->size();
        if (t >= kSlowPairCheckSec)
        {
            outs() << "[UAF][pair-check-slow] source=" << slice->getSource()->getId()
                   << " freeNodes=" << freeSet->size()
                   << " useNodes=" << useSet->size()
                   << " pairChecks=" << pairChecks
                   << " feasible=" << feasiblePairs
                   << " time=" << t
                   << " hit=" << (flag ? 0 : 1)
                   << "\n";
            outs().flush();
        }
    }

    return flag;
}

void UseAfterFreeChecker::reportBug(ProgSlice* slice)
{
    const bool timeStat = Options::SaberTimeStat();
    double reportStart = 0;
    if (timeStat)
    {
        reportStart = SVFStat::getClk(true);
        ++saberTimeStat.uafReportCalls;
    }

    GenericBug::EventStack eventStack;

    if(!isSatisfiableForFreeAndUsePairs(slice, eventStack))
    {
        eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, getSrcCSID(slice->getSource())));
        queueUAFReport(std::move(eventStack), "alloc_source");
    }

    if (timeStat)
    {
        const double reportTime = (SVFStat::getClk(true) - reportStart) / TIMEINTERVAL;
        saberTimeStat.uafReportTime += reportTime;
        if (reportTime >= kSlowSourceReportSec)
        {
            outs() << "[UAF][source-slow] source=" << slice->getSource()->getId()
                   << " reportTime=" << reportTime
                   << " forwardSlice=" << slice->getForwardSliceSize()
                   << " sinks=" << slice->getSinks().size()
                   << "\n";
            outs().flush();
        }
    }
}

const CallICFGNode* UseAfterFreeChecker::getFreeCallICFGNode(const SVFGNode* freeNode)
{
    const ICFGNode* icfg = freeNode->getICFGNode();
    if (icfg == nullptr)
        return nullptr;
    return SVFUtil::dyn_cast<CallICFGNode>(icfg);
}

bool UseAfterFreeChecker::isDuplicateUAFPair(const SVFGNode* freeNode,
        const SVFGNode* useNode) const
{
    if (freeNode == nullptr || useNode == nullptr)
        return false;
    return reportedUAFPairs_.find(std::make_pair(freeNode->getId(), useNode->getId())) !=
           reportedUAFPairs_.end();
}

void UseAfterFreeChecker::markUAFPairReported(const SVFGNode* freeNode,
        const SVFGNode* useNode)
{
    if (freeNode == nullptr || useNode == nullptr)
        return;
    reportedUAFPairs_.insert(std::make_pair(freeNode->getId(), useNode->getId()));
}

bool UseAfterFreeChecker::isDirectCallerUseOfFreeFunction(const SVFGNode* freeNode,
        const SVFGNode* useNode) const
{
    const ICFGNode* freeICFG = freeNode->getICFGNode();
    const ICFGNode* useICFG = useNode->getICFGNode();
    if (freeICFG == nullptr || useICFG == nullptr)
        return false;

    const FunObjVar* callee = freeICFG->getFun();
    const FunObjVar* useFun = useICFG->getFun();
    if (callee == nullptr || useFun == nullptr || callee == useFun)
        return false;

    SVFIR* pag = getPAG();
    for (SVFIR::CSToArgsListMap::iterator it = pag->getCallSiteArgsMap().begin(),
            eit = pag->getCallSiteArgsMap().end(); it != eit; ++it)
    {
        const CallICFGNode* cs = it->first;
        if (cs->getFun() != useFun)
            continue;

        CallGraph::FunctionSet callees;
        getCallgraph()->getCallees(cs, callees);
        if (callees.find(callee) != callees.end())
            return true;
    }
    return false;
}

bool UseAfterFreeChecker::isUseAfterFreeCallInCaller(const SVFGNode* freeNode,
        const SVFGNode* useNode) const
{
    const ICFGNode* freeICFG = freeNode->getICFGNode();
    const ICFGNode* useICFG = useNode->getICFGNode();
    if (freeICFG == nullptr || useICFG == nullptr)
        return false;

    const FunObjVar* callee = freeICFG->getFun();
    const FunObjVar* useFun = useICFG->getFun();
    if (callee == nullptr || useFun == nullptr || callee == useFun)
        return false;

    SVFIR* pag = getPAG();
    for (SVFIR::CSToArgsListMap::iterator it = pag->getCallSiteArgsMap().begin(),
            eit = pag->getCallSiteArgsMap().end(); it != eit; ++it)
    {
        const CallICFGNode* cs = it->first;
        if (cs->getFun() != useFun)
            continue;

        CallGraph::FunctionSet callees;
        getCallgraph()->getCallees(cs, callees);
        if (callees.find(callee) == callees.end())
            continue;

        const ICFGNode* retICFG = cs->getRetICFGNode();
        if (retICFG != nullptr && icfgReachable(retICFG, useICFG))
            return true;
    }

    return icfgReachable(freeICFG, useICFG);
}

bool UseAfterFreeChecker::isOrderedValueFlowUAFPair(const SVFGNode* freeNode,
        const SVFGNode* useNode) const
{
    const ICFGNode* freeICFG = freeNode == nullptr ? nullptr : freeNode->getICFGNode();
    const ICFGNode* useICFG = useNode == nullptr ? nullptr : useNode->getICFGNode();
    if (freeICFG == nullptr || useICFG == nullptr)
        return false;

    const FunObjVar* freeFun = freeICFG->getFun();
    const FunObjVar* useFun = useICFG->getFun();
    if (freeFun == nullptr || useFun == nullptr)
        return false;

    if (freeFun == useFun)
        return sameFunctionFreeOrdersUse(freeICFG, useICFG);

    PointerAnalysis* pta = getSVFG() == nullptr ? nullptr : getSVFG()->getPTA();
    if (pta == nullptr)
        return false;

    const SVFVar* freed = getFreedPointerVar(freeNode);
    if (freed == nullptr)
        return false;

    SVFIR* pag = getPAG();
    if (const std::vector<UAFDirectCallSiteInfo>* callsites =
                lookupDirectCalls(pag, getCallgraph(), useFun, freeFun))
    {
        for (const UAFDirectCallSiteInfo& info : *callsites)
        {
            const ICFGNode* retICFG = info.cs->getRetICFGNode();
            if (!sameFunctionAcyclicReachable(retICFG, useICFG))
                continue;

            for (const UAFIndexedCallArg& arg : info.pointerArgs)
            {
                if (actualArgMatchesFunctionArg(pta, arg.var, freed, arg.index, freeFun) &&
                        actualArgMatchesUse(pta, arg.var, useNode, arg.index, useFun))
                    return true;
            }
        }
    }

    if (const std::vector<UAFDirectCallSiteInfo>* callsites =
                lookupDirectCalls(pag, getCallgraph(), freeFun, useFun))
    {
        for (const UAFDirectCallSiteInfo& info : *callsites)
        {
            if (!sameFunctionAcyclicReachable(freeICFG, info.cs))
                continue;

            for (const UAFIndexedCallArg& arg : info.pointerArgs)
            {
                if (pointerVarsMayAlias(pta, arg.var, freed) &&
                        actualArgMatchesUse(pta, arg.var, useNode, arg.index, useFun))
                    return true;
            }
        }
    }

    return false;
}

bool UseAfterFreeChecker::isFieldLoadUseOfFreedBase(const SVFGNode* freeNode,
        const SVFGNode* useNode) const
{
    const LoadVFGNode* ld = SVFUtil::dyn_cast<LoadVFGNode>(useNode);
    if (ld == nullptr)
        return false;

    const SVFVar* freed = getFreedPointerVar(freeNode);
    const SVFVar* loadPtr = ld->getPAGSrcNode();
    if (freed == nullptr || loadPtr == nullptr)
        return false;

    PointerAnalysis* pta = getSVFG()->getPTA();
    if (pta == nullptr)
        return false;

    if (pagVarHasGepInChain(loadPtr, 12))
    {
        const SVFVar* loadBase = getGepBaseValVar(loadPtr);
        if (pointerVarsMayAlias(pta, freed, loadBase))
            return true;
    }

    if (getGepBaseValVar(loadPtr) != loadPtr)
    {
        const SVFVar* loadBase = getGepBaseValVar(loadPtr);
        if (pointerVarsMayAlias(pta, freed, loadBase))
            return true;
    }

    return false;
}

bool UseAfterFreeChecker::isNestedFieldFreePointer(const SVFGNode* freeNode) const
{
    const SVFVar* freed = getFreedPointerVar(freeNode);
    if (freed == nullptr)
        return false;
    if (SVFUtil::isa<GepValVar>(freed))
        return true;

    SVFVar* mut = const_cast<SVFVar*>(freed);
    for (const SVFStmt* edge : mut->getIncomingEdges(SVFStmt::Gep))
    {
        (void)edge;
        return true;
    }

    if (SVFUtil::isa<ArgValVar>(freed))
    {
        for (const SVFStmt* edge : mut->getIncomingEdges(SVFStmt::Copy))
        {
            if (pagVarHasGepInChain(edge->getSrcNode(), 6))
                return true;
        }
        return false;
    }

    if (pagVarHasGepInChain(freed, 12))
        return true;

    return getGepBaseValVar(freed) != freed;
}

bool UseAfterFreeChecker::shouldSkipCrossFileAllocNoise(const SVFGNode* freeNode,
        const SVFGNode* useNode) const
{
    const ICFGNode* freeICFG = freeNode->getICFGNode();
    const ICFGNode* useICFG = useNode->getICFGNode();
    if (freeICFG == nullptr || useICFG == nullptr)
        return false;

    if (freeICFG->getFun() == useICFG->getFun())
        return false;

    std::string freeFile;
    std::string useFile;
    u32_t dummyLine = 0;
    SaberSliceExportUtil::parseLocFileLine(freeICFG->getSourceLoc(), freeFile, dummyLine);
    SaberSliceExportUtil::parseLocFileLine(useICFG->getSourceLoc(), useFile, dummyLine);

    if (freeFile.empty() || useFile.empty() || freeFile == useFile)
        return false;

    return !isDirectCallerUseOfFreeFunction(freeNode, useNode);
}

void UseAfterFreeChecker::tryReportCrossFunctionUAF(const SVFGNode* freeNode,
        const SVFGNode* useNode)
{
    if (isDuplicateUAFPair(freeNode, useNode))
        return;
    if (!isUseAfterFreeCallInCaller(freeNode, useNode))
        return;
    if (!isDirectCallerUseOfFreeFunction(freeNode, useNode))
        return;
    if (isNestedFieldFreePointer(freeNode))
        return;
    if (!maySameFreedObject(freeNode, useNode))
        return;
    if (!SVFUtil::isa<LoadVFGNode>(useNode))
        return;

    ProgSlice guardSlice(freeNode, getSaberCondAllocator(), getSVFG());
    ProgSlice::Condition orderCond =
        computeControlOrderGuard(&guardSlice, freeNode, useNode);
    if (guardSlice.isEquivalentBranchCond(orderCond, guardSlice.getFalseCond()))
        return;

    const CallICFGNode* srcCS = getFreeCallICFGNode(freeNode);
    if (srcCS == nullptr)
        return;

    GenericBug::EventStack eventStack;
    eventStack.push_back(SVFBugEvent(SVFBugEvent::Free, freeNode->getICFGNode()));
    eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, useNode->getICFGNode()));
    eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, srcCS));
    queueUAFReport(std::move(eventStack), "cross_function");
    markUAFPairReported(freeNode, useNode);
}

std::vector<const SVFGNode*> UseAfterFreeChecker::getCallerActualParmAnchors(
    const SVFGNode* freeNode) const
{
    std::vector<const SVFGNode*> anchors;
    const ICFGNode* freeICFG = freeNode->getICFGNode();
    if (freeICFG == nullptr)
        return anchors;

    const FunObjVar* containerFun = freeICFG->getFun();
    if (containerFun == nullptr)
        return anchors;

    SVFIR* pag = getPAG();
    const SVFG* svfg = getSVFG();
    for (SVFIR::CSToArgsListMap::iterator it = pag->getCallSiteArgsMap().begin(),
            eit = pag->getCallSiteArgsMap().end(); it != eit; ++it)
    {
        const CallICFGNode* cs = it->first;
        CallGraph::FunctionSet callees;
        getCallgraph()->getCallees(cs, callees);
        if (callees.find(containerFun) == callees.end())
            continue;

        for (const PAGNode* pagNode : it->second)
        {
            if (!pagNode->isPointer())
                continue;
            const SVFGNode* ap = svfg->getActualParmVFGNode(pagNode, cs);
            if (ap == nullptr)
                continue;
            anchors.push_back(ap);
            break;
        }
    }
    return anchors;
}

void UseAfterFreeChecker::analyzeFreeAnchoredUAFPairs()
{
    UAFUsePtsIndex useIndex;
    PointerAnalysis* pta = getSVFG() == nullptr ? nullptr : getSVFG()->getPTA();
    buildUAFUsePtsIndex(useNodes, pta, useIndex);

    u64_t localCandidates = 0;
    u64_t localOrderMatches = 0;
    u64_t localAliasMatches = 0;
    u64_t localGuardMatches = 0;
    u64_t vfCandidates = 0;
    u64_t vfAliasMatches = 0;
    u64_t vfGuardMatches = 0;
    for (SVFGNodeSetIter fit = freeNodes.begin(), efit = freeNodes.end(); fit != efit; ++fit)
    {
        const SVFGNode* freeNode = *fit;
        const ICFGNode* ficfg = freeNode->getICFGNode();
        if (ficfg == nullptr)
            continue;
        const FunObjVar* freeFun = ficfg->getFun();
        if (freeFun == nullptr)
            continue;

        PointsTo freedPts;
        collectAliasPtsForVar(pta, getFreedPointerVar(freeNode), freedPts);

        std::vector<const SVFGNode*> valueFlowUses;
        if (!freedPts.empty())
            collectReturnPrunedValueFlowUses(freeNode, useNodes, freedPts, 5000,
                                             Options::SaberUAFReachMaxNodes(), valueFlowUses);
        for (const SVFGNode* useNode : valueFlowUses)
        {
            ++vfCandidates;
            if (isDuplicateUAFPair(freeNode, useNode))
                continue;
            if (!maySameFreedObject(freeNode, useNode))
                continue;

            const ICFGNode* uicfg = useNode->getICFGNode();
            if (uicfg == nullptr)
                continue;
            // The free must actually be able to reach the use in the CFG. The old
            // `isLaterSourceLineInSameFile` fallback only compared source line numbers,
            // which admits free/use on MUTUALLY-EXCLUSIVE branches (e.g. kfree in the
            // error path, use in the success path) -- a large FP source on kernel code.
            // Require real intra-procedural reachability instead.
            if (!isOrderedValueFlowUAFPair(freeNode, useNode) &&
                    !sameFunctionAcyclicReachable(ficfg, uicfg))
                continue;
            if (freedPointerReassignedBeforeUse(getFreedPointerVar(freeNode), ficfg, uicfg))
                continue;
            ++vfAliasMatches;

            ProgSlice guardSlice(freeNode, getSaberCondAllocator(), getSVFG());
            ProgSlice::Condition orderCond =
                computeControlOrderGuard(&guardSlice, freeNode, useNode);
            if (guardSlice.isEquivalentBranchCond(orderCond, guardSlice.getFalseCond()))
                continue;
            ++vfGuardMatches;

            const CallICFGNode* srcCS = getFreeCallICFGNode(freeNode);
            if (srcCS == nullptr)
                continue;

            GenericBug::EventStack eventStack;
            eventStack.push_back(SVFBugEvent(SVFBugEvent::Free, ficfg));
            eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, uicfg));
            eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, srcCS));
            queueUAFReport(std::move(eventStack), "return_pruned_vfg");
            markUAFPairReported(freeNode, useNode);
        }

        std::vector<const SVFGNode*> candidateUses;
        collectObjectCandidateUsesFromIndex(useIndex, pta, getFreedPointerVar(freeNode),
                                            useNodes, candidateUses);
        if (candidateUses.empty())
            candidateUses.insert(candidateUses.end(), useNodes.begin(), useNodes.end());

        for (const SVFGNode* useNode : candidateUses)
        {
            ++localCandidates;
            if (isDuplicateUAFPair(freeNode, useNode))
                continue;

            const ICFGNode* uicfg = useNode->getICFGNode();
            if (uicfg == nullptr)
                continue;

            if (!maySameFreedObject(freeNode, useNode))
                continue;
            ++localAliasMatches;
            if (!isOrderedValueFlowUAFPair(freeNode, useNode))
                continue;
            ++localOrderMatches;
            if (freedPointerReassignedBeforeUse(getFreedPointerVar(freeNode), ficfg, uicfg))
                continue;

            ProgSlice guardSlice(freeNode, getSaberCondAllocator(), getSVFG());
            ProgSlice::Condition orderCond =
                computeControlOrderGuard(&guardSlice, freeNode, useNode);
            if (guardSlice.isEquivalentBranchCond(orderCond, guardSlice.getFalseCond()))
                continue;
            ++localGuardMatches;

            const CallICFGNode* srcCS = getFreeCallICFGNode(freeNode);
            if (srcCS == nullptr)
                continue;

            GenericBug::EventStack eventStack;
            eventStack.push_back(SVFBugEvent(SVFBugEvent::Free, ficfg));
            eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, uicfg));
            eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, srcCS));
            queueUAFReport(std::move(eventStack), "local_ordered");
            markUAFPairReported(freeNode, useNode);
        }
    }
    if (Options::UAFCheck())
    {
        outs() << "[UAF][local-fastpath] candidates=" << localCandidates
               << " orderMatches=" << localOrderMatches
               << " aliasMatches=" << localAliasMatches
               << " guardMatches=" << localGuardMatches
               << " vfCandidates=" << vfCandidates
               << " vfAliasMatches=" << vfAliasMatches
               << " vfGuardMatches=" << vfGuardMatches
               << "\n";
        outs().flush();
    }
}

void UseAfterFreeChecker::queueUAFReport(GenericBug::EventStack eventStack,
        const std::string& reportKind)
{
    const ICFGNode* freeICFG = nullptr;
    const ICFGNode* useICFG = nullptr;
    for (const SVFBugEvent& ev : eventStack)
    {
        if (ev.getEventType() == SVFBugEvent::Free)
            freeICFG = ev.getEventInst();
        else if (ev.getEventType() == SVFBugEvent::Use)
            useICFG = ev.getEventInst();
    }

    // Authoritative system/STL/generated-code filter: drop any UAF report whose
    // free or use anchor lands in system headers (e.g. <stop_token>, <bits/invoke.h>),
    // libstdc++/libc, or generated code. This is the single choke point shared by all
    // report kinds (return_pruned_vfg / local_ordered / cross_function / alloc_source),
    // so it catches frees whose location is attributed to inlined STL code.
    if (uafIsSystemOrGeneratedCodeICFG(freeICFG) ||
            uafIsSystemOrGeneratedCodeICFG(useICFG))
        return;

    // Drop degenerate pairs whose USE has no source location (compiler-generated /
    // synthetic nodes, printed as "use=()" / ":0"). A real use-after-free has a
    // concrete use site; a locationless use is never actionable and is pure noise.
    {
        std::string uFile; u32_t uLine = 0;
        if (!getICFGSourceFileLine(useICFG, uFile, uLine))
            return;
    }

    SaberPendingReport pending;
    pending.bugType = GenericBug::USEAFTERFREE;
    pending.eventStack = std::move(eventStack);
    pending.uafReportKind = reportKind;
    if (!queuePendingReport(std::move(pending),
                            SaberSliceExportUtil::makeICFGPairLocKey(freeICFG, useICFG)))
        return;
    ++saberTimeStat.uafReportedSources;
}

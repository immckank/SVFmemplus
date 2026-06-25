// svf/lib/SABER/UseAfterFreeChecker.cpp
#include "SABER/UseAfterFreeChecker.h"
#include "SABER/SaberSliceExport.h"
#include "SABER/SaberCheckerAPI.h"
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
                addToSinks(getSVFG()->getStmtVFGNode(ld));
                addToUseNodes(getSVFG()->getStmtVFGNode(ld));
            }
        }
        for(const SVFStmt* st : var->getIncomingEdges(SVFStmt::Store))
        {
            if(getSVFG()->hasStmtVFGNode(st)){
                addToSinks(getSVFG()->getStmtVFGNode(st));
                addToUseNodes(getSVFG()->getStmtVFGNode(st));
            }
        }
    }
}

static bool icfgReachable(const ICFGNode* start, const ICFGNode* target);

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
    for (SVFIR::CSToArgsListMap::iterator it = pag->getCallSiteArgsMap().begin(),
            eit = pag->getCallSiteArgsMap().end(); it != eit; ++it)
    {
        const CallICFGNode* cs = it->first;
        if (cs->getFun() != useFun)
            continue;

        CallGraph::FunctionSet callees;
        getCallgraph()->getCallees(cs, callees);
        if (callees.find(freeFun) == callees.end())
            continue;

        for (const PAGNode* pagNode : it->second)
        {
            if (!pagNode->isPointer())
                continue;
            const SVFVar* actualArg = SVFUtil::cast<SVFVar>(pagNode);
            if (pointerVarsMayAlias(pta, actualArg, useBase))
                return true;
            break;
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

bool isBackEdge(const ICFGEdge* edge) {
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
    double nextProgressTime = 0;
    if (progress || timeStat)
    {
        totalStart = SVFStat::getClk(true);
        nextProgressTime = totalStart + 5 * TIMEINTERVAL;
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

    u32_t sourceIndex = 0;
    for (SVFGNodeSetIter iter = sourcesBegin(), eiter = sourcesEnd();
            iter != eiter; ++iter)
    {
        ++sourceIndex;
        runSliceFromSource(*iter, false);

        if (progress)
        {
            const double now = SVFStat::getClk(true);
            if (now >= nextProgressTime || sourceIndex == numSrcs)
            {
                outs() << "[UAF][source-progress] index=" << sourceIndex
                       << "/" << numSrcs
                       << " elapsed=" << (now - totalStart) / TIMEINTERVAL
                       << " withSinks=" << saberTimeStat.uafSourcesWithSinks
                       << " reported=" << saberTimeStat.uafReportedSources
                       << "\n";
                outs().flush();
                nextProgressTime = now + 5 * TIMEINTERVAL;
            }
        }
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
        for (SVFGNodeSetIter uit = useSet->begin(), euit = useSet->end(); uit != euit; ++uit)
        {
            const SVFGNode* useNode = *uit;
            for (SVFGNodeSetIter fit = freeSet->begin(), efit = freeSet->end(); fit != efit; ++fit)
            {
                const SVFGNode* freeNode = *fit;
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

            for (const SVFGNode* useNode : reachableUses)
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
    std::unordered_map<const ICFGNode*, std::vector<const SVFGNode*>> usesByICFGNode;
    size_t totalIndexedUses = 0;
    usesByICFGNode.reserve(useNodes.size());
    for (SVFGNodeSetIter uit = useNodes.begin(), euit = useNodes.end(); uit != euit; ++uit)
    {
        const ICFGNode* uicfg = (*uit)->getICFGNode();
        if (uicfg == nullptr)
            continue;
        usesByICFGNode[uicfg].push_back(*uit);
        ++totalIndexedUses;
    }

    const size_t maxVisitedNodes = Options::SaberUAFReachMaxNodes();
    for (SVFGNodeSetIter fit = freeNodes.begin(), efit = freeNodes.end(); fit != efit; ++fit)
    {
        const SVFGNode* freeNode = *fit;
        const ICFGNode* ficfg = freeNode->getICFGNode();
        if (ficfg == nullptr)
            continue;

        std::vector<const SVFGNode*> reachableUses;
        collectReachableUsesFromFree(ficfg, usesByICFGNode, totalIndexedUses,
                                     maxVisitedNodes, reachableUses);
        if (reachableUses.empty())
            continue;

        bool hasNewPair = false;
        for (const SVFGNode* useNode : reachableUses)
        {
            if (isDuplicateUAFPair(freeNode, useNode))
                continue;
            if (!maySameFreedObject(freeNode, useNode))
                continue;
            hasNewPair = true;
            break;
        }

        const FunObjVar* freeFun = ficfg->getFun();
        if (!hasNewPair && freeFun != nullptr)
        {
            const std::vector<const SVFGNode*> callerAnchors =
                getCallerActualParmAnchors(freeNode);
            if (!callerAnchors.empty())
            {
                for (const SVFGNode* useNode : reachableUses)
                {
                    if (isDuplicateUAFPair(freeNode, useNode))
                        continue;
                    const ICFGNode* uicfg = useNode->getICFGNode();
                    if (uicfg != nullptr && uicfg->getFun() != freeFun)
                    {
                        hasNewPair = true;
                        break;
                    }
                }
            }
        }

        if (!hasNewPair)
            continue;

        bool hasCrossFunctionUse = false;
        for (const SVFGNode* useNode : reachableUses)
        {
            const ICFGNode* uicfg = useNode->getICFGNode();
            if (uicfg == nullptr)
                continue;
            if (uicfg->getFun() != freeFun)
            {
                hasCrossFunctionUse = true;
                break;
            }
        }

        if (hasCrossFunctionUse)
        {
            for (const SVFGNode* useNode : reachableUses)
            {
                const ICFGNode* uicfg = useNode->getICFGNode();
                if (uicfg == nullptr || uicfg->getFun() == freeFun)
                    continue;
                tryReportCrossFunctionUAF(freeNode, useNode);
            }
            continue;
        }

        runSliceFromSource(freeNode, true);
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

    SaberPendingReport pending;
    pending.bugType = GenericBug::USEAFTERFREE;
    pending.eventStack = std::move(eventStack);
    pending.uafReportKind = reportKind;
    if (!queuePendingReport(std::move(pending),
                            SaberSliceExportUtil::makeICFGPairLocKey(freeICFG, useICFG)))
        return;
    ++saberTimeStat.uafReportedSources;
}

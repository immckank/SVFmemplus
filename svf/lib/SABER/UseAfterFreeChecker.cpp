// svf/lib/SABER/UseAfterFreeChecker.cpp
#include "SABER/UseAfterFreeChecker.h"
#include "SABER/SaberCheckerAPI.h"
#include "SVFIR/SVFIR.h"
#include "Util/SVFUtil.h"
#include "Util/SVFStat.h"
#include "Util/Options.h"
#include "SABER/ProgSlice.h"

using namespace SVF;
using namespace SVFUtil;

static constexpr double kSlowSourceReportSec = 1.0;
static constexpr double kSlowPairCheckSec = 3.0;

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
        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Call))
        {
            if(getSVFG()->hasStmtVFGNode(ld)){
                addToSinks(getSVFG()->getStmtVFGNode(ld));
                addToUseNodes(getSVFG()->getStmtVFGNode(ld));
            }
        }
    }
}

bool icfgReachable(const ICFGNode* start, const ICFGNode* target) {
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

    ContextCond::setMaxCxtLen(Options::CxtLimit());

    const u32_t numSrcs = getSources().size();
    if (progress)
    {
        outs() << "[UAF][analyze-begin] sources=" << numSrcs << "\n";
        outs().flush();
    }

    u32_t sourceIndex = 0;
    for (SVFGNodeSetIter iter = sourcesBegin(), eiter = sourcesEnd();
            iter != eiter; ++iter)
    {
        ++sourceIndex;
        setCurSlice(*iter);

        ContextCond cxt;
        DPIm item((*iter)->getId(), cxt);

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

        if (!getCurSlice()->getSinks().empty())
            ++saberTimeStat.uafSourcesWithSinks;

        reportBug(getCurSlice());

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

    if (progress || timeStat)
    {
        const double elapsed = (SVFStat::getClk(true) - totalStart) / TIMEINTERVAL;
        if (timeStat)
            saberTimeStat.totalTime = elapsed;
        if (progress)
        {
            outs() << "[UAF][analyze-done] sources=" << numSrcs
                   << " withSinks=" << saberTimeStat.uafSourcesWithSinks
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
    for(SVFGNodeSetIter fit = freeSet->begin(), efit = freeSet->end(); fit!=efit; ++fit)
    {
        for(SVFGNodeSetIter uit = useSet->begin(), euit = useSet->end(); uit!=euit; ++uit)
        {
            ++pairChecks;
            ProgSlice::Condition guard = slice->condAnd(slice->getVFCond(*fit),slice->getVFCond(*uit));
            if(!slice->isEquivalentBranchCond(guard, slice->getFalseCond()))
            {
                const ICFGNode* ficfg = (*fit)->getICFGNode();
                const ICFGNode* uicfg = (*uit)->getICFGNode();
                if(!icfgReachable(ficfg, uicfg)) continue;

                eventStack.push_back(SVFBugEvent(SVFBugEvent::Free, (*fit)->getICFGNode()));
                slice->setFinalCond(slice->getVFCond(*fit));
                slice->evalFinalCond2Event(eventStack);
                eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, (*uit)->getICFGNode()));
                slice->setFinalCond(slice->getVFCond(*uit));
                slice->evalFinalCond2Event(eventStack);
                flag = false;
                ++feasiblePairs;

                if(hasLoopBackEdge(ficfg, uicfg)) eventStack.push_back(SVFBugEvent(SVFBugEvent::PotentialLoop, ficfg));
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
        report.addSaberBug(GenericBug::USEAFTERFREE, eventStack);
        ++saberTimeStat.uafReportedSources;
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

// svf/lib/SABER/UninitChecker.cpp
#include "SABER/UninitChecker.h"
#include "SABER/SaberCheckerAPI.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFType.h"
#include "Util/SVFStat.h"
#include "Util/SVFUtil.h"
#include "SABER/ProgSlice.h"
#include "Util/WorkList.h"
#include "SVFIR/SVFValue.h"
#include "Graphs/ICFG.h"
#include <deque>
#include <unordered_set>

using namespace SVF;
using namespace SVFUtil;

typedef VisitedFIFOWorkList<const SVFGNode*> BackwardWorkList;
typedef FIFOWorkList<const SVFGNode*> ForwardWorkList;

static const ICFGNode* getBugEventICFGNode(const SVFGNode* node)
{
    return node == nullptr ? nullptr : node->getICFGNode();
}

static bool hasUsableSourceLoc(const ICFGNode* node)
{
    return node != nullptr && node->getSourceLoc().empty() == false;
}

static int icfgNodeKindPenalty(const ICFGNode* node)
{
    if (node == nullptr)
        return 100;
    if (SVFUtil::isa<IntraICFGNode>(node))
        return 0;
    if (SVFUtil::isa<CallICFGNode>(node) || SVFUtil::isa<RetICFGNode>(node))
        return 2;
    if (SVFUtil::isa<FunEntryICFGNode>(node) || SVFUtil::isa<FunExitICFGNode>(node))
        return 5;
    return 10;
}

static const ICFGNode* findNearbySourceICFGNode(const SVFGNode* source, u32_t maxDepth, u32_t maxVisited)
{
    if (source == nullptr)
        return nullptr;

    std::deque<std::pair<const SVFGNode*, u32_t>> worklist;
    std::unordered_set<const SVFGNode*> visited;
    worklist.push_back(std::make_pair(source, 0));
    visited.insert(source);

    const ICFGNode* bestAtCurrentDepth = nullptr;
    u32_t currentDepth = 0;
    u32_t visitedCount = 0;

    while (!worklist.empty() && visitedCount < maxVisited)
    {
        const SVFGNode* node = worklist.front().first;
        const u32_t depth = worklist.front().second;
        worklist.pop_front();
        ++visitedCount;

        if (depth > maxDepth)
            continue;

        if (depth != currentDepth)
        {
            if (bestAtCurrentDepth)
                return bestAtCurrentDepth;
            currentDepth = depth;
        }

        const ICFGNode* icfgNode = getBugEventICFGNode(node);
        if (hasUsableSourceLoc(icfgNode))
        {
            if (bestAtCurrentDepth == nullptr ||
                    icfgNodeKindPenalty(icfgNode) < icfgNodeKindPenalty(bestAtCurrentDepth))
            {
                bestAtCurrentDepth = icfgNode;
            }
        }

        if (depth == maxDepth)
            continue;

        for (const auto* edge : node->getInEdges())
        {
            const SVFGNode* pred = edge->getSrcNode();
            if (visited.insert(pred).second)
                worklist.push_back(std::make_pair(pred, depth + 1));
        }

        for (const auto* edge : node->getOutEdges())
        {
            const SVFGNode* succ = edge->getDstNode();
            if (visited.insert(succ).second)
                worklist.push_back(std::make_pair(succ, depth + 1));
        }
    }

    return bestAtCurrentDepth;
}

static const ICFGNode* findFallbackICFGFromEvents(const GenericBug::EventStack& eventStack)
{
    for (auto it = eventStack.rbegin(); it != eventStack.rend(); ++it)
    {
        if (it->getEventType() == SVFBugEvent::Use && hasUsableSourceLoc(it->getEventInst()))
            return it->getEventInst();
    }

    for (auto it = eventStack.rbegin(); it != eventStack.rend(); ++it)
    {
        if (hasUsableSourceLoc(it->getEventInst()))
            return it->getEventInst();
    }

    return nullptr;
}

static const ICFGNode* findFunctionLevelFallbackICFG(const SVFGNode* source, SVFIR* pag)
{
    const ICFGNode* sourceICFG = getBugEventICFGNode(source);
    if (sourceICFG == nullptr || sourceICFG->getFun() == nullptr || pag == nullptr || pag->getICFG() == nullptr)
        return sourceICFG;

    if (FunEntryICFGNode* funEntry = pag->getICFG()->getFunEntryICFGNode(sourceICFG->getFun()))
        return funEntry;

    return sourceICFG;
}

static const ICFGNode* selectBestSourceICFGNode(const SVFGNode* source, const GenericBug::EventStack& eventStack, SVFIR* pag)
{
    // P0: original source if it already has usable source location.
    const ICFGNode* sourceICFG = getBugEventICFGNode(source);
    if (hasUsableSourceLoc(sourceICFG))
        return sourceICFG;

    // P1: nearest reachable SVFG node with non-empty source location.
    if (const ICFGNode* nearby = findNearbySourceICFGNode(source, 8, 200))
        return nearby;

    // P2: use nearest event location in current bug path (prefer Use events).
    if (const ICFGNode* fromEvents = findFallbackICFGFromEvents(eventStack))
        return fromEvents;

    // P3: function-level fallback to keep a stable, non-empty anchor.
    return findFunctionLevelFallbackICFG(source, pag);
}

UninitChecker::UninitChecker() : LeakChecker() {
    Options::SaberKeepDerefDirSVFGEdges.setValue(true);
    Options::SABERFULLSVFG.setValue(true);
}
UninitChecker::~UninitChecker() { }

bool UninitChecker::runOnModule(SVFIR* pag) {
    analyze();
    return false;
}

void UninitChecker::analyze()
{
    const bool timeStat = Options::SaberTimeStat();
    double totalStart = 0;
    if (timeStat)
        totalStart = SVFStat::getClk(true);

    initialize();

    ContextCond::setMaxCxtLen(Options::CxtLimit());

    u32_t sourceIndex = 0;
    for (SVFGNodeSetIter iter = sourcesBegin(), eiter = sourcesEnd();
            iter != eiter; ++iter)
    {
        ++sourceIndex;
        setCurSlice(*iter);

        ContextCond cxt;
        DPIm item((*iter)->getId(), cxt);

        if (timeStat)
        {
            outs() << "[UNINIT][source-begin] index=" << sourceIndex
                   << "/" << saberTimeStat.numSrcs
                   << " id=" << (*iter)->getId() << "\n";
            outs().flush();
        }

        double fwdStart = 0;
        if (timeStat)
            fwdStart = SVFStat::getClk(true);
        forwardTraverse(item);
        if (timeStat)
        {
            saberTimeStat.forwardTraverseTime += (SVFStat::getClk(true) - fwdStart) / TIMEINTERVAL;
            saberTimeStat.numSinks += getCurSlice()->getSinks().size();
            if (getCurSlice()->getForwardSliceSize() > saberTimeStat.uninitMaxForwardSlice)
                saberTimeStat.uninitMaxForwardSlice = getCurSlice()->getForwardSliceSize();

            outs() << "[UNINIT][forward-done] index=" << sourceIndex
                   << " id=" << (*iter)->getId()
                   << " time=" << (SVFStat::getClk(true) - fwdStart) / TIMEINTERVAL
                   << " forwardSlice=" << getCurSlice()->getForwardSliceSize()
                   << " reachedLoads=" << getCurSlice()->getSinks().size() << "\n";
            outs().flush();
        }

        double reportStart = 0;
        if (timeStat)
            reportStart = SVFStat::getClk(true);
        reportBug(getCurSlice());
        if (timeStat)
        {
            outs() << "[UNINIT][source-done] index=" << sourceIndex
                   << " id=" << (*iter)->getId()
                   << " reportTime=" << (SVFStat::getClk(true) - reportStart) / TIMEINTERVAL
                   << "\n";
            outs().flush();
        }
    }

    if (timeStat)
        saberTimeStat.totalTime = (SVFStat::getClk(true) - totalStart) / TIMEINTERVAL;
    finalize();
}

void UninitChecker::initSrcs()
{
    SVFIR* pag = getPAG();
    const bool timeStat = Options::SaberTimeStat();
    double start = 0;
    u32_t varCount = 0;
    u32_t addrStmtCount = 0;
    if (timeStat)
    {
        start = SVFStat::getClk(true);
        outs() << "[UNINIT][init-srcs-begin] pagVars=" << pag->getTotalNodeNum() << "\n";
        outs().flush();
    }

    summaryBoundaryToLoads.clear();
    summaryBoundaryToBoundaries.clear();
    ignorePtrStoreForLoadCache.clear();

    for (SVFIR::iterator it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        ++varCount;
        SVFVar* var = it->second;

        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Addr))
        {
            ++addrStmtCount;
            if(getSVFG()->hasStmtVFGNode(ld)){
                SVFGNode* addrVFGNode = getSVFG()->getStmtVFGNode(ld);
                
                if(addrVFGNode->toString().find("alloca") != std::string::npos){
                    addToSources(getSVFG()->getStmtVFGNode(ld));
                }
            }
        }
        if (timeStat && varCount % 10000 == 0)
        {
            outs() << "[UNINIT][init-srcs-progress] vars=" << varCount
                   << " addrStmts=" << addrStmtCount
                   << " sources=" << getSources().size()
                   << " elapsed=" << (SVFStat::getClk(true) - start) / TIMEINTERVAL << "\n";
            outs().flush();
        }
            
    }
    if (timeStat)
    {
        outs() << "[UNINIT][init-srcs-done] vars=" << varCount
               << " addrStmts=" << addrStmtCount
               << " sources=" << getSources().size()
               << " elapsed=" << (SVFStat::getClk(true) - start) / TIMEINTERVAL << "\n";
        outs().flush();
    }
}

/*!
 * Initialize sinks
 */
void UninitChecker::initSnks()
{
    SVFIR* pag = getPAG();
    const bool timeStat = Options::SaberTimeStat();
    double start = 0;
    u32_t varCount = 0;
    u32_t storeStmtCount = 0;
    u32_t loadStmtCount = 0;
    if (timeStat)
    {
        start = SVFStat::getClk(true);
        outs() << "[UNINIT][init-sinks-begin] pagVars=" << pag->getTotalNodeNum() << "\n";
        outs().flush();
    }

    storeNodes.clear();
    loadNodes.clear();
    ptrStoreNodes.clear();
    ptrLoadNodes.clear();
    ignorePtrStoreForLoadCache.clear();

    for (SVFIR::iterator it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        ++varCount;
        SVFVar* var = it->second;

        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Store))
        {
            ++storeStmtCount;
            if(getSVFG()->hasStmtVFGNode(ld)){
                const SVFGNode* storeNode = getSVFG()->getStmtVFGNode(ld);
                addToStoreNodes(storeNode);
                if (storeNode->toString().find("store ptr") != std::string::npos)
                    ptrStoreNodes.insert(storeNode);
            }
        }
        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Load))
        {   
            ++loadStmtCount;
            if(getSVFG()->hasStmtVFGNode(ld)){
                const SVFGNode* loadNode = getSVFG()->getStmtVFGNode(ld);
                addToSinks(loadNode);
                addToLoadNodes(loadNode);
                if (loadNode->toString().find("load ptr") != std::string::npos)
                    ptrLoadNodes.insert(loadNode);
            }
        }
        if (timeStat && varCount % 10000 == 0)
        {
            outs() << "[UNINIT][init-sinks-progress] vars=" << varCount
                   << " stores=" << storeStmtCount
                   << " loads=" << loadStmtCount
                   << " storeNodes=" << storeNodes.size()
                   << " loadNodes=" << loadNodes.size()
                   << " elapsed=" << (SVFStat::getClk(true) - start) / TIMEINTERVAL << "\n";
            outs().flush();
        }
            
    }
    if (timeStat)
    {
        outs() << "[UNINIT][init-sinks-done] vars=" << varCount
               << " stores=" << storeStmtCount
               << " loads=" << loadStmtCount
               << " storeNodes=" << storeNodes.size()
               << " loadNodes=" << loadNodes.size()
               << " ptrStores=" << ptrStoreNodes.size()
               << " ptrLoads=" << ptrLoadNodes.size()
               << " elapsed=" << (SVFStat::getClk(true) - start) / TIMEINTERVAL << "\n";
        outs().flush();
    }
}

bool UninitChecker::isPtrStoreNode(const SVFGNode* node) const
{
    return ptrStoreNodes.find(node) != ptrStoreNodes.end();
}

bool UninitChecker::isPtrLoadNode(const SVFGNode* node) const
{
    return ptrLoadNodes.find(node) != ptrLoadNodes.end();
}

bool UninitChecker::shouldConsiderStoreForLoad(const SVFGNode* load, const SVFGNode* store, ProgSlice* slice) const
{
    return shouldConsiderStoreForMode(store, slice, shouldIgnorePtrStoreForLoad(load));
}

bool UninitChecker::isLoadCoveredByStores(ProgSlice* guardSlice,
                                          const SVFGNode* load,
                                          const SVFGNodeSet& curStoreSet) const
{
    auto& solver = ProgSlice::Condition::getSolver();
    solver.push();
    solver.add(guardSlice->getVFCond(load).getExpr());

    for (SVFGNodeSetIter sit = curStoreSet.begin(), esit = curStoreSet.end(); sit != esit; ++sit)
        solver.add(!guardSlice->getVFCond(*sit).getExpr());

    z3::check_result res = solver.check();
    solver.pop();
    return res == z3::unsat;
}

bool UninitChecker::shouldConsiderStoreForSummaryMode(const SVFGNode* node, bool ignorePtrStore) const
{
    if (storeNodes.find(node) == storeNodes.end())
        return false;
    if (!ignorePtrStore)
        return true;
    return !isPtrStoreNode(node);
}

bool UninitChecker::isSummaryBoundaryNode(const SVFGNode* node) const
{
    return ActualParmVFGNode::classof(node) || ActualRetVFGNode::classof(node) ||
           FormalParmVFGNode::classof(node) || FormalRetVFGNode::classof(node) ||
           ActualINSVFGNode::classof(node) || ActualOUTSVFGNode::classof(node) ||
           FormalINSVFGNode::classof(node) || FormalOUTSVFGNode::classof(node);
}

u64_t UninitChecker::getSummaryKey(const SVFGNode* node, bool ignorePtrStore) const
{
    u64_t key = static_cast<u64_t>(node->getId());
    if (ignorePtrStore)
        key |= (1ULL << 63);
    return key;
}

void UninitChecker::getOrBuildSummaryForBoundary(const SVFGNode* boundary, bool ignorePtrStore,
                                                 const SVFGNodeSet*& reachableLoads,
                                                 const SVFGNodeSet*& nextBoundaries)
{
    u64_t key = getSummaryKey(boundary, ignorePtrStore);
    auto itLoad = summaryBoundaryToLoads.find(key);
    auto itBoundary = summaryBoundaryToBoundaries.find(key);
    if (itLoad != summaryBoundaryToLoads.end() && itBoundary != summaryBoundaryToBoundaries.end())
    {
        reachableLoads = &itLoad->second;
        nextBoundaries = &itBoundary->second;
        return;
    }

    SVFGNodeSet localLoads;
    SVFGNodeSet localBoundaries;
    SVFGNodeSet visited;
    ForwardWorkList workList;

    visited.insert(boundary);
    workList.push(boundary);

    while (!workList.empty())
    {
        const SVFGNode* node = workList.pop();

        for (auto edge : node->getOutEdges())
        {
            const SVFGNode* succ = edge->getDstNode();
            if (shouldConsiderStoreForSummaryMode(succ, ignorePtrStore))
                continue;

            if (loadNodes.find(succ) != loadNodes.end())
                localLoads.insert(succ);

            if (succ != boundary && isSummaryBoundaryNode(succ))
            {
                localBoundaries.insert(succ);
                continue;
            }

            if (visited.insert(succ).second)
                workList.push(succ);
        }
    }

    summaryBoundaryToLoads[key] = localLoads;
    summaryBoundaryToBoundaries[key] = localBoundaries;
    reachableLoads = &summaryBoundaryToLoads[key];
    nextBoundaries = &summaryBoundaryToBoundaries[key];
}

bool UninitChecker::shouldIgnorePtrStoreForLoad(const SVFGNode* load) const
{
    auto cached = ignorePtrStoreForLoadCache.find(load);
    if (cached != ignorePtrStoreForLoadCache.end())
        return cached->second;

    bool ignorePtrStore = true;
    if (!isPtrLoadNode(load))
    {
        ignorePtrStoreForLoadCache[load] = ignorePtrStore;
        return true;
    }

    for (auto edge : load->getOutEdges())
    {
        SVFGNode* next = edge->getDstNode();
        if (ActualParmVFGNode::classof(next) && next->getOutEdges().empty())
        {
            ignorePtrStoreForLoadCache[load] = ignorePtrStore;
            return true;
        }
    }
    ignorePtrStore = false;
    ignorePtrStoreForLoadCache[load] = ignorePtrStore;
    return ignorePtrStore;
}

bool UninitChecker::shouldConsiderStoreForMode(const SVFGNode* store, ProgSlice* slice, bool ignorePtrStore) const
{
    if (!ignorePtrStore)
        return true;

    if (!isPtrStoreNode(store))
        return true;

    bool formal_param = false;
    bool cur_alloc = false;
    for (auto edge : store->getInEdges())
    {
        SVFGNode* pre = edge->getSrcNode();
        if (FormalParmVFGNode::classof(pre))
            formal_param = true;
        else if (pre == slice->getSource())
            cur_alloc = true;
    }
    return cur_alloc && formal_param;
}

bool UninitChecker::inUninitCandidateSlice(ProgSlice* slice, const SVFGNode* node) const
{
    return slice->inBackwardSlice(node) || slice->inForwardSlice(node);
}

void UninitChecker::computeQualifierInferenceState(ProgSlice* slice, bool ignorePtrStore, SVFGNodeSet& mayUninitReachable)
{
    mayUninitReachable.clear();
    const SVFGNode* source = slice->getSource();
    ForwardWorkList workList;

    workList.push(source);

    while (!workList.empty())
    {
        const SVFGNode* node = workList.pop();
        if (!inUninitCandidateSlice(slice, node))
            continue;
        if (!mayUninitReachable.insert(node).second)
            continue;

        if (isSummaryBoundaryNode(node))
        {
            const SVFGNodeSet* summaryLoads = nullptr;
            const SVFGNodeSet* summaryBoundaries = nullptr;
            getOrBuildSummaryForBoundary(node, ignorePtrStore, summaryLoads, summaryBoundaries);

            for (SVFGNodeSetIter lit = summaryLoads->begin(), elit = summaryLoads->end(); lit != elit; ++lit)
            {
                if (inUninitCandidateSlice(slice, *lit))
                    mayUninitReachable.insert(*lit);
            }
            for (SVFGNodeSetIter bit = summaryBoundaries->begin(), ebit = summaryBoundaries->end(); bit != ebit; ++bit)
            {
                if (inUninitCandidateSlice(slice, *bit))
                    workList.push(*bit);
            }
            continue;
        }

        for (auto edge : node->getOutEdges())
        {
            const SVFGNode* succ = edge->getDstNode();
            if (!inUninitCandidateSlice(slice, succ))
                continue;
            if (shouldConsiderStoreForSummaryMode(succ, ignorePtrStore))
                continue;
            workList.push(succ);
        }
    }

}

bool UninitChecker::isDefinitelyInitInComputedState(const SVFGNodeSet& mayUninitReachable, const SVFGNode* load) const
{
    return mayUninitReachable.find(load) == mayUninitReachable.end();
}

void UninitChecker::collectCandidateLoads(const SVFGNodeSet& qualifierStateIgnorePtrStore,
                                          const SVFGNodeSet& qualifierStateAllStore,
                                          SVFGNodeSet& candidateLoads) const
{
    candidateLoads.clear();
    for (SVFGNodeSetIter lit = qualifierStateIgnorePtrStore.begin(), elit = qualifierStateIgnorePtrStore.end(); lit != elit; ++lit)
    {
        const SVFGNode* load = *lit;
        if (loadNodes.find(load) == loadNodes.end())
            continue;
        bool ignorePtrStore = shouldIgnorePtrStoreForLoad(load);
        const SVFGNodeSet& qualifierState = ignorePtrStore ? qualifierStateIgnorePtrStore : qualifierStateAllStore;
        if (!isDefinitelyInitInComputedState(qualifierState, load))
            candidateLoads.insert(load);
    }
}

std::unique_ptr<ProgSlice> UninitChecker::buildGuardSlice(ProgSlice* rawSlice,
                                                           const SVFGNodeSet& candidateLoads) const
{
    auto guardSlice = std::make_unique<ProgSlice>(rawSlice->getSource(), getSaberCondAllocator(), getSVFG());
    BackwardWorkList backwardWorkList;
    SVFGNodeSet reducedBackward;

    for (SVFGNodeSetIter lit = candidateLoads.begin(), elit = candidateLoads.end(); lit != elit; ++lit)
    {
        const SVFGNode* load = *lit;
        if (!inUninitCandidateSlice(rawSlice, load))
            continue;
        guardSlice->addToSinks(load);
        backwardWorkList.push(load);
    }

    if (guardSlice->getSinks().empty())
        return nullptr;

    guardSlice->setPartialReachable();

    while (!backwardWorkList.empty())
    {
        const SVFGNode* node = backwardWorkList.pop();
        if (!inUninitCandidateSlice(rawSlice, node))
            continue;
        if (!reducedBackward.insert(node).second)
            continue;

        for (auto edge : node->getInEdges())
        {
            const SVFGNode* pre = edge->getSrcNode();
            if (inUninitCandidateSlice(rawSlice, pre))
                backwardWorkList.push(pre);
        }
    }

    for (SVFGNodeSetIter it = reducedBackward.begin(), eit = reducedBackward.end(); it != eit; ++it)
        guardSlice->addToBackwardSlice(*it);

    return guardSlice;
}

bool UninitChecker::isSatisfiableForLoads(ProgSlice* rawSlice, ProgSlice* guardSlice,
                                          const SVFGNodeSet& candidateLoads,
                                          const SVFGNodeSet& qualifierStateIgnorePtrStore,
                                          const SVFGNodeSet& qualifierStateAllStore,
                                          GenericBug::EventStack& eventStack){
    const bool timeStat = Options::SaberTimeStat();
    double checkStart = 0;
    if (timeStat)
    {
        checkStart = SVFStat::getClk(true);
        outs() << "[UNINIT][load-check-begin] source=" << rawSlice->getSource()->getId()
               << " candidates=" << candidateLoads.size()
               << " guardBackward=" << guardSlice->getBackwardSliceSize() << "\n";
        outs().flush();
    }

    u32_t checkedLoads = 0;
    for(SVFGNodeSetIter lit = candidateLoads.begin(), elit = candidateLoads.end(); lit!=elit; ++lit){
        const SVFGNode* load = *lit;
        bool ignorePtrStore = shouldIgnorePtrStoreForLoad(load);
        const SVFGNodeSet& qualifierState = ignorePtrStore ? qualifierStateIgnorePtrStore : qualifierStateAllStore;
        if(isDefinitelyInitInComputedState(qualifierState, load))
            continue;
        if(!guardSlice->inBackwardSlice(load))
            continue;
        ++checkedLoads;
        if (timeStat && checkedLoads % 100 == 0)
        {
            outs() << "[UNINIT][load-check-progress] source=" << rawSlice->getSource()->getId()
                   << " checked=" << checkedLoads
                   << " elapsed=" << (SVFStat::getClk(true) - checkStart) / TIMEINTERVAL
                   << "\n";
            outs().flush();
        }

        SVFGNodeSet curStoreSet;

        BackwardWorkList backwardWorkList;
        backwardWorkList.push(load);
        const u32_t maxBackwardSteps = Options::SaberUninitMaxBackwardSteps();
        u32_t backwardSteps = 0;

        while (!backwardWorkList.empty())
        {
            if (backwardSteps >= maxBackwardSteps)
                break;

            const SVFGNode* node = backwardWorkList.pop();
            ++backwardSteps;
            // if(!slice->inBackwardSlice(node)) continue;

            for(auto edge : node->getInEdges()){
                SVFGNode* pre = edge->getSrcNode();
                if(guardSlice->inBackwardSlice(pre)) backwardWorkList.push(pre);
            }

            if(storeNodes.find(node) != storeNodes.end() && shouldConsiderStoreForLoad(load, node, rawSlice))
                curStoreSet.insert(node);
        }


        if(!isLoadCoveredByStores(guardSlice, load, curStoreSet)){
            if (const ICFGNode* loadICFG = getBugEventICFGNode(load))
                eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, loadICFG));
            if (timeStat)
            {
                double t = (SVFStat::getClk(true) - checkStart) / TIMEINTERVAL;
                saberTimeStat.uninitLoadCheckTime += t;
                outs() << "[UNINIT][load-check-hit] source=" << rawSlice->getSource()->getId()
                       << " load=" << load->getId()
                       << " checked=" << checkedLoads
                       << " stores=" << curStoreSet.size()
                       << " time=" << t << "\n";
                outs().flush();
            }
            return false;
        }
    }
    if (timeStat)
    {
        double t = (SVFStat::getClk(true) - checkStart) / TIMEINTERVAL;
        saberTimeStat.uninitLoadCheckTime += t;
        outs() << "[UNINIT][load-check-done] source=" << rawSlice->getSource()->getId()
               << " checked=" << checkedLoads
               << " time=" << t << "\n";
        outs().flush();
    }
    return true;
}


void UninitChecker::reportBug(ProgSlice* rawSlice)
{
    const bool timeStat = Options::SaberTimeStat();
    double reportStart = 0;
    if (timeStat)
    {
        reportStart = SVFStat::getClk(true);
        ++saberTimeStat.uninitReportCalls;
        outs() << "[UNINIT][report-begin] source=" << rawSlice->getSource()->getId()
               << " forwardSlice=" << rawSlice->getForwardSliceSize()
               << " reachedLoads=" << rawSlice->getSinks().size() << "\n";
        outs().flush();
    }

    auto finishReport = [&]() {
        if (timeStat)
            saberTimeStat.uninitReportTime += (SVFStat::getClk(true) - reportStart) / TIMEINTERVAL;
    };

    SVFGNodeSet qualifierStateIgnorePtrStore;
    SVFGNodeSet qualifierStateAllStore;

    double phaseStart = 0;
    if (timeStat)
    {
        phaseStart = SVFStat::getClk(true);
        outs() << "[UNINIT][qualifier-begin] source=" << rawSlice->getSource()->getId() << "\n";
        outs().flush();
    }
    computeQualifierInferenceState(rawSlice, true, qualifierStateIgnorePtrStore);
    computeQualifierInferenceState(rawSlice, false, qualifierStateAllStore);
    if (timeStat)
    {
        double t = (SVFStat::getClk(true) - phaseStart) / TIMEINTERVAL;
        saberTimeStat.uninitQualifierTime += t;
        outs() << "[UNINIT][qualifier-done] source=" << rawSlice->getSource()->getId()
               << " ignorePtrReachable=" << qualifierStateIgnorePtrStore.size()
               << " allStoreReachable=" << qualifierStateAllStore.size()
               << " time=" << t << "\n";
        outs().flush();
    }

    SVFGNodeSet candidateLoads;
    if (timeStat)
        phaseStart = SVFStat::getClk(true);
    collectCandidateLoads(qualifierStateIgnorePtrStore, qualifierStateAllStore, candidateLoads);
    if (timeStat)
    {
        double t = (SVFStat::getClk(true) - phaseStart) / TIMEINTERVAL;
        saberTimeStat.uninitCollectCandidateTime += t;
        saberTimeStat.uninitTotalCandidateLoads += candidateLoads.size();
        if (candidateLoads.size() > saberTimeStat.uninitMaxCandidateLoads)
            saberTimeStat.uninitMaxCandidateLoads = candidateLoads.size();
        outs() << "[UNINIT][candidate-done] source=" << rawSlice->getSource()->getId()
               << " candidates=" << candidateLoads.size()
               << " time=" << t << "\n";
        outs().flush();
    }
    if (candidateLoads.empty())
    {
        finishReport();
        return;
    }
    if (timeStat)
        ++saberTimeStat.uninitSourcesWithCandidates;

    if (timeStat)
    {
        phaseStart = SVFStat::getClk(true);
        outs() << "[UNINIT][guard-build-begin] source=" << rawSlice->getSource()->getId()
               << " candidates=" << candidateLoads.size() << "\n";
        outs().flush();
    }
    std::unique_ptr<ProgSlice> guardSlice = buildGuardSlice(rawSlice, candidateLoads);
    if (!guardSlice)
    {
        if (timeStat)
        {
            double t = (SVFStat::getClk(true) - phaseStart) / TIMEINTERVAL;
            saberTimeStat.uninitGuardBuildTime += t;
            outs() << "[UNINIT][guard-build-empty] source=" << rawSlice->getSource()->getId()
                   << " time=" << t << "\n";
            outs().flush();
        }
        finishReport();
        return;
    }
    if (timeStat)
    {
        double t = (SVFStat::getClk(true) - phaseStart) / TIMEINTERVAL;
        saberTimeStat.uninitGuardBuildTime += t;
        if (guardSlice->getBackwardSliceSize() > saberTimeStat.uninitMaxGuardBackwardSlice)
            saberTimeStat.uninitMaxGuardBackwardSlice = guardSlice->getBackwardSliceSize();
        outs() << "[UNINIT][guard-build-done] source=" << rawSlice->getSource()->getId()
               << " guardSinks=" << guardSlice->getSinks().size()
               << " guardBackward=" << guardSlice->getBackwardSliceSize()
               << " time=" << t << "\n";
        outs().flush();
    }

    double solveStart = 0;
    if (Options::SaberTimeStat())
    {
        solveStart = SVFStat::getClk(true);
        outs() << "[UNINIT][guard-solve-begin] source=" << rawSlice->getSource()->getId()
               << " guardBackward=" << guardSlice->getBackwardSliceSize() << "\n";
        outs().flush();
    }
    guardSlice->AllPathReachableSolve(false);
    if (Options::SaberTimeStat())
    {
        double t = (SVFStat::getClk(true) - solveStart) / TIMEINTERVAL;
        addSolveTime(t);
        saberTimeStat.uninitGuardSolveTime += t;
        outs() << "[UNINIT][guard-solve-done] source=" << rawSlice->getSource()->getId()
               << " time=" << t << "\n";
        outs().flush();
    }

    GenericBug::EventStack eventStack;
    if(!isSatisfiableForLoads(rawSlice, guardSlice.get(), candidateLoads,
                              qualifierStateIgnorePtrStore, qualifierStateAllStore, eventStack))
    {
        const ICFGNode* sourceICFG = selectBestSourceICFGNode(rawSlice->getSource(), eventStack, getPAG());
        if (sourceICFG == nullptr)
        {
            finishReport();
            return;
        }
        eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, sourceICFG));
        report.addSaberBug(GenericBug::UNINIT, eventStack);
        if (timeStat)
            ++saberTimeStat.uninitReportedSources;
    }
    finishReport();
}

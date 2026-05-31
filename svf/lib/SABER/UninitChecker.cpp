// svf/lib/SABER/UninitChecker.cpp
#include "SABER/UninitChecker.h"
#include "SABER/SaberCheckerAPI.h"
#include "SVFIR/SVFIR.h"
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

/*
 * Bug reports want a stable "source" location. However, a raw SVFG source node
 * (e.g., an allocation-related node) may not carry user-facing debug info.
 *
 * The helpers below implement a conservative selection strategy:
 * - Prefer a real source location closest to the SVFG source.
 * - Prefer statement-level ICFG nodes over call/ret and function entry/exit.
 * - Fall back to an event location in the current path if needed.
 * - As a last resort, anchor at the containing function entry.
 */
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

    // Smaller penalty means "better" for a user-facing anchor location.
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

    // Breadth-first exploration from the source in the SVFG, limited by depth and visited budget.
    // We return as soon as we finish the first depth level that yields at least one usable source location.
    std::deque<std::pair<const SVFGNode*, u32_t>> worklist;
    std::unordered_set<const SVFGNode*> visited;
    worklist.push_back(std::make_pair(source, 0));
    visited.insert(source);

    const ICFGNode* bestAtCurrentDepth = nullptr;
    int bestPenaltyAtCurrentDepth = 0;
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
            const int penalty = icfgNodeKindPenalty(icfgNode);
            if (bestAtCurrentDepth == nullptr || penalty < bestPenaltyAtCurrentDepth)
            {
                bestAtCurrentDepth = icfgNode;
                bestPenaltyAtCurrentDepth = penalty;
            }
        }

        if (depth == maxDepth)
            continue;

        // Traverse both in-edges and out-edges to find a nearby node with usable debug info.
        // This intentionally treats the SVFG neighborhood as "undirected" for anchoring.
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
    // Prefer a concrete "Use" site if present (more actionable than generic control-flow anchors).
    for (auto it = eventStack.rbegin(); it != eventStack.rend(); ++it)
    {
        if (it->getEventType() == SVFBugEvent::Use && hasUsableSourceLoc(it->getEventInst()))
            return it->getEventInst();
    }

    // Otherwise return the latest event in the path that still has debug info.
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
    // P0: Source node already carries a usable source location.
    const ICFGNode* sourceICFG = getBugEventICFGNode(source);
    if (hasUsableSourceLoc(sourceICFG))
        return sourceICFG;

    // P1: Find the closest reachable SVFG node (within a small bounded BFS) that has debug info.
    if (const ICFGNode* nearby = findNearbySourceICFGNode(source, 8, 200))
        return nearby;

    // P2: Fall back to the current event path (prefer a Use site).
    if (const ICFGNode* fromEvents = findFallbackICFGFromEvents(eventStack))
        return fromEvents;

    // P3: Function-level fallback for a stable anchor when all else is missing.
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

void UninitChecker::initSrcs()
{
    SVFIR* pag = getPAG();


    for (SVFIR::iterator it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        SVFVar* var = it->second;

        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Addr))
        {
            if(getSVFG()->hasStmtVFGNode(ld)){
                SVFGNode* addrVFGNode = getSVFG()->getStmtVFGNode(ld);
                
                if(addrVFGNode->toString().find("alloca") != std::string::npos){
                    addToSources(getSVFG()->getStmtVFGNode(ld));
                }
            }
        }
            
    }
}

/*!
 * Initialize sinks
 */
void UninitChecker::initSnks()
{
    SVFIR* pag = getPAG();


    for (SVFIR::iterator it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        SVFVar* var = it->second;

        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Store))
        {
            if(getSVFG()->hasStmtVFGNode(ld)){
                addToStoreNodes(getSVFG()->getStmtVFGNode(ld));
            }
        }
        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Load))
        {   
            if(getSVFG()->hasStmtVFGNode(ld)){
                addToSinks(getSVFG()->getStmtVFGNode(ld));
                addToLoadNodes(getSVFG()->getStmtVFGNode(ld));
            }
        }
            
    }
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
    return node->toString().find("store ptr") == std::string::npos;
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

void UninitChecker::getOrBuildSummaryForBoundary(const SVFGNode* boundary, bool ignorePtrStore, SVFGNodeSet& reachableLoads, SVFGNodeSet& nextBoundaries)
{
    u64_t key = getSummaryKey(boundary, ignorePtrStore);
    auto itLoad = summaryBoundaryToLoads.find(key);
    auto itBoundary = summaryBoundaryToBoundaries.find(key);
    if (itLoad != summaryBoundaryToLoads.end() && itBoundary != summaryBoundaryToBoundaries.end())
    {
        reachableLoads = itLoad->second;
        nextBoundaries = itBoundary->second;
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
    reachableLoads = summaryBoundaryToLoads[key];
    nextBoundaries = summaryBoundaryToBoundaries[key];
}

bool UninitChecker::shouldIgnorePtrStoreForLoad(const SVFGNode* load) const
{
    if (load->toString().find("load ptr") == std::string::npos)
        return true;

    for (auto edge : load->getOutEdges())
    {
        SVFGNode* next = edge->getDstNode();
        if (ActualParmVFGNode::classof(next) && next->getOutEdges().empty())
            return true;
    }
    return false;
}

bool UninitChecker::shouldConsiderStoreForMode(const SVFGNode* store, ProgSlice* slice, bool ignorePtrStore) const
{
    if (!ignorePtrStore)
        return true;

    if (store->toString().find("store ptr") == std::string::npos)
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

void UninitChecker::computeQualifierInferenceState(ProgSlice* slice, bool ignorePtrStore, SVFGNodeSet& mayUninitReachable)
{
    mayUninitReachable.clear();
    const SVFGNode* source = slice->getSource();
    ForwardWorkList workList;

    workList.push(source);

    while (!workList.empty())
    {
        const SVFGNode* node = workList.pop();
        if (!slice->inBackwardSlice(node))
            continue;
        if (!mayUninitReachable.insert(node).second)
            continue;

        if (isSummaryBoundaryNode(node))
        {
            SVFGNodeSet summaryLoads;
            SVFGNodeSet summaryBoundaries;
            getOrBuildSummaryForBoundary(node, ignorePtrStore, summaryLoads, summaryBoundaries);

            for (SVFGNodeSetIter lit = summaryLoads.begin(), elit = summaryLoads.end(); lit != elit; ++lit)
            {
                if (slice->inBackwardSlice(*lit))
                    mayUninitReachable.insert(*lit);
            }
            for (SVFGNodeSetIter bit = summaryBoundaries.begin(), ebit = summaryBoundaries.end(); bit != ebit; ++bit)
            {
                if (slice->inBackwardSlice(*bit))
                    workList.push(*bit);
            }
            continue;
        }

        for (auto edge : node->getOutEdges())
        {
            const SVFGNode* succ = edge->getDstNode();
            if (!slice->inBackwardSlice(succ))
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
        if (!rawSlice->inBackwardSlice(load))
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
        if (!rawSlice->inBackwardSlice(node))
            continue;
        if (!reducedBackward.insert(node).second)
            continue;

        for (auto edge : node->getInEdges())
        {
            const SVFGNode* pre = edge->getSrcNode();
            if (rawSlice->inBackwardSlice(pre))
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
    bool flag = true;

    for(SVFGNodeSetIter lit = candidateLoads.begin(), elit = candidateLoads.end(); lit!=elit; ++lit){
        const SVFGNode* load = *lit;
        bool ignorePtrStore = shouldIgnorePtrStoreForLoad(load);
        const SVFGNodeSet& qualifierState = ignorePtrStore ? qualifierStateIgnorePtrStore : qualifierStateAllStore;
        if(isDefinitelyInitInComputedState(qualifierState, load))
            continue;
        if(!guardSlice->inBackwardSlice(load))
            continue;

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
            flag = false;
            if (const ICFGNode* loadICFG = getBugEventICFGNode(load))
                eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, loadICFG));
        }
    }
    return flag;
}


void UninitChecker::reportBug(ProgSlice* rawSlice)
{
    SVFGNodeSet qualifierStateIgnorePtrStore;
    SVFGNodeSet qualifierStateAllStore;
    computeQualifierInferenceState(rawSlice, true, qualifierStateIgnorePtrStore);
    computeQualifierInferenceState(rawSlice, false, qualifierStateAllStore);

    SVFGNodeSet candidateLoads;
    collectCandidateLoads(qualifierStateIgnorePtrStore, qualifierStateAllStore, candidateLoads);
    if (candidateLoads.empty())
        return;

    std::unique_ptr<ProgSlice> guardSlice = buildGuardSlice(rawSlice, candidateLoads);
    if (!guardSlice)
        return;

    guardSlice->AllPathReachableSolve(false);

    GenericBug::EventStack eventStack;
    if(!isSatisfiableForLoads(rawSlice, guardSlice.get(), candidateLoads,
                              qualifierStateIgnorePtrStore, qualifierStateAllStore, eventStack))
    {
        const ICFGNode* sourceICFG = selectBestSourceICFGNode(rawSlice->getSource(), eventStack, getPAG());
        if (sourceICFG == nullptr)
            return;
        eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, sourceICFG));
        report.addSaberBug(GenericBug::UNINIT, eventStack);
    }
}

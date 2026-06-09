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
static constexpr double kSlowSourceReportSec = 1.0;
static constexpr double kSlowPathCheckSec = 3.0;

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

void UninitChecker::analyze()
{
    const bool timeStat = Options::SaberTimeStat();
    double totalStart = 0;
    double nextProgressTime = 0;
    if (timeStat)
    {
        totalStart = SVFStat::getClk(true);
        nextProgressTime = totalStart + 5 * TIMEINTERVAL;
    }

    initialize();

    ContextCond::setMaxCxtLen(Options::CxtLimit());

    if (timeStat)
    {
        outs() << "[UNINIT][analyze-begin] sources=" << saberTimeStat.numSrcs << "\n";
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
            if (getCurSlice()->getForwardSliceSize() > saberTimeStat.uninitMaxForwardSlice)
                saberTimeStat.uninitMaxForwardSlice = getCurSlice()->getForwardSliceSize();

        }

        if (getCurSlice()->getSinks().empty())
            continue;

        reportBug(getCurSlice());
        if (timeStat)
        {
            const double now = SVFStat::getClk(true);
            if (now >= nextProgressTime || sourceIndex == saberTimeStat.numSrcs)
            {
                outs() << "[UNINIT][source-progress] index=" << sourceIndex
                       << "/" << saberTimeStat.numSrcs
                       << " elapsed=" << (now - totalStart) / TIMEINTERVAL
                       << " withCandidates=" << saberTimeStat.uninitSourcesWithCandidates
                       << " reported=" << saberTimeStat.uninitReportedSources
                       << "\n";
                outs().flush();
                nextProgressTime = now + 5 * TIMEINTERVAL;
            }
        }
    }

    if (timeStat)
    {
        saberTimeStat.totalTime = (SVFStat::getClk(true) - totalStart) / TIMEINTERVAL;
        outs() << "[UNINIT][analyze-done] sources=" << saberTimeStat.numSrcs
               << " withCandidates=" << saberTimeStat.uninitSourcesWithCandidates
               << " reported=" << saberTimeStat.uninitReportedSources
               << " elapsed=" << saberTimeStat.totalTime << "\n";
        outs().flush();
    }
    finalize();
}

bool UninitChecker::isParameterSpillStackObject(StackObjVar* stackObj, SVFIR* pag) const
{
    const FunObjVar* fun = stackObj->getFunction();
    if (fun == nullptr)
        return false;

    // By-value aggregate parameters may carry uninitialized bytes from the caller.
    if (stackObj->isVarStruct() || stackObj->isVarArray() || stackObj->isArray())
        return false;

    if (!stackObj->hasOutgoingEdges(SVFStmt::Addr))
        return false;

    bool hasSpillSlot = false;
    for (const SVFStmt* addrStmt : stackObj->getOutgoingEdges(SVFStmt::Addr))
    {
        const SVFVar* slotPtr = addrStmt->getDstNode();
        if (slotPtr == nullptr || !slotPtr->hasIncomingEdges(SVFStmt::Store))
            return false;

        hasSpillSlot = true;
        for (SVFStmt::SVFStmtSetTy::const_iterator sit = slotPtr->getIncomingEdgesBegin(SVFStmt::Store),
                seit = slotPtr->getIncomingEdgesEnd(SVFStmt::Store); sit != seit; ++sit)
        {
            const SVFVar* rhs = SVFUtil::cast<StoreStmt>(*sit)->getRHSVar();
            const ArgValVar* arg = SVFUtil::dyn_cast<ArgValVar>(rhs);
            if (arg == nullptr || arg->getFunction() != fun)
                return false;
        }
    }

    return hasSpillSlot;
}

void UninitChecker::initSrcs()
{
    SVFIR* pag = getPAG();

    summaryBoundaryToLoads.clear();
    summaryBoundaryToBoundaries.clear();
    ignorePtrStoreForLoadCache.clear();

    for (SVFIR::iterator it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        SVFVar* var = it->second;
        if (!var->hasOutgoingEdges(SVFStmt::Addr))
            continue;

        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Addr))
        {
            if(getSVFG()->hasStmtVFGNode(ld)){
                SVFVar* obj = const_cast<SVFVar*>(ld->getSrcNode());
                if (StackObjVar* stackObj = SVFUtil::dyn_cast<StackObjVar>(obj))
                {
                    if (isParameterSpillStackObject(stackObj, pag))
                        continue;
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

    storeNodes.clear();
    loadNodes.clear();
    ptrStoreNodes.clear();
    ptrLoadNodes.clear();
    ignorePtrStoreForLoadCache.clear();

    for (SVFIR::iterator it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        SVFVar* var = it->second;

        if (var->hasOutgoingEdges(SVFStmt::Store))
        {
            for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Store))
            {
                if(getSVFG()->hasStmtVFGNode(ld)){
                    const SVFGNode* storeNode = getSVFG()->getStmtVFGNode(ld);
                    addToStoreNodes(storeNode);
                    if (ld->getSrcNode()->isPointer())
                        ptrStoreNodes.insert(storeNode);
                }
            }
        }
        if (var->hasOutgoingEdges(SVFStmt::Load))
        {
            for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Load))
            {   
                if(getSVFG()->hasStmtVFGNode(ld)){
                    const SVFGNode* loadNode = getSVFG()->getStmtVFGNode(ld);
                    addToSinks(loadNode);
                    addToLoadNodes(loadNode);
                    if (ld->getDstNode()->isPointer())
                        ptrLoadNodes.insert(loadNode);
                }
            }
        }
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
    if (storeNodes.find(store) == storeNodes.end())
        return false;

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

bool UninitChecker::isFormalParameterPointerLoad(const SVFGNode* load) const
{
    if (!isPtrLoadNode(load))
        return false;

    for (auto edge : load->getInEdges())
    {
        const SVFGNode* pred = edge->getSrcNode();
        if (FormalParmVFGNode::classof(pred))
            return true;
        if (const InterPHIVFGNode* phi = SVFUtil::dyn_cast<InterPHIVFGNode>(pred))
        {
            if (phi->isFormalParmPHI())
                return true;
        }
    }
    return false;
}

bool UninitChecker::isDirectParameterSpillLoad(const SVFGNode* load) const
{
    if (!SVFUtil::isa<LoadSVFGNode>(load))
        return false;

    for (auto edge : load->getInEdges())
    {
        if (SVFUtil::isa<GepSVFGNode>(edge->getSrcNode()))
            return false;
    }

    BackwardWorkList worklist;
    worklist.push(load);
    SVFGNodeSet visited;
    u32_t steps = 0;
    constexpr u32_t kMaxSpillTraceSteps = 8;

    while (!worklist.empty() && steps++ < kMaxSpillTraceSteps)
    {
        const SVFGNode* node = worklist.pop();
        if (!visited.insert(node).second)
            continue;

        if (FormalParmVFGNode::classof(node))
            return true;
        if (const InterPHIVFGNode* phi = SVFUtil::dyn_cast<InterPHIVFGNode>(node))
        {
            if (phi->isFormalParmPHI())
                return true;
        }

        for (auto edge : node->getInEdges())
        {
            const SVFGNode* pred = edge->getSrcNode();
            if (FormalParmVFGNode::classof(pred))
                return true;

            if (const StoreSVFGNode* store = SVFUtil::dyn_cast<StoreSVFGNode>(pred))
            {
                for (auto storeIn : store->getInEdges())
                {
                    if (FormalParmVFGNode::classof(storeIn->getSrcNode()))
                        return true;
                }
            }

            if (SVFUtil::isa<AddrSVFGNode>(pred) || SVFUtil::isa<CopySVFGNode>(pred) ||
                    SVFUtil::isa<PHISVFGNode>(pred) ||
                    (SVFUtil::isa<LoadSVFGNode>(pred) && isPtrLoadNode(pred)))
                worklist.push(pred);
        }
    }
    return false;
}

bool UninitChecker::isPtrLoadAddressComputationOnly(const SVFGNode* load) const
{
    if (!isPtrLoadNode(load))
        return false;

    ForwardWorkList worklist;
    worklist.push(load);
    SVFGNodeSet visited;

    while (!worklist.empty())
    {
        const SVFGNode* node = worklist.pop();
        if (!visited.insert(node).second)
            continue;

        if (node != load && loadNodes.find(node) != loadNodes.end() && !isPtrLoadNode(node))
            return false;

        for (auto edge : node->getOutEdges())
        {
            const SVFGNode* succ = edge->getDstNode();

            if (loadNodes.find(succ) != loadNodes.end())
            {
                worklist.push(succ);
            }
            else if (storeNodes.find(succ) != storeNodes.end() ||
                     SVFUtil::isa<GepSVFGNode>(succ) ||
                     SVFUtil::isa<CopySVFGNode>(succ) ||
                     SVFUtil::isa<PHISVFGNode>(succ))
            {
                worklist.push(succ);
            }
            else if (ActualParmVFGNode::classof(succ) || isSummaryBoundaryNode(succ))
            {
                continue;
            }
            else
            {
                return false;
            }
        }
    }
    return true;
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
        // Loads from spilled formal-parameter slots, or pointer loads only used for
        // address computation, do not read uninitialized pointee bytes.
        if (isDirectParameterSpillLoad(load) || isFormalParameterPointerLoad(load) ||
                isPtrLoadAddressComputationOnly(load))
            continue;
        bool ignorePtrStore = shouldIgnorePtrStoreForLoad(load);
        const SVFGNodeSet& qualifierState = ignorePtrStore ? qualifierStateIgnorePtrStore : qualifierStateAllStore;
        if (!isDefinitelyInitInComputedState(qualifierState, load))
            candidateLoads.insert(load);
    }
}

std::unique_ptr<ProgSlice> UninitChecker::buildStoreBypassGuardSlice(ProgSlice* rawSlice,
                                                                     const SVFGNode* load) const
{
    auto guardSlice = std::make_unique<ProgSlice>(rawSlice->getSource(), getSaberCondAllocator(), getSVFG());
    BackwardWorkList backwardWorkList;
    SVFGNodeSet reducedBackward;

    if (load == nullptr || !inUninitCandidateSlice(rawSlice, load))
        return nullptr;

    guardSlice->addToSinks(load);
    backwardWorkList.push(load);

    guardSlice->setPartialReachable();

    while (!backwardWorkList.empty())
    {
        const SVFGNode* node = backwardWorkList.pop();
        if (!inUninitCandidateSlice(rawSlice, node))
            continue;
        if (node != load && shouldConsiderStoreForLoad(load, node, rawSlice))
            continue;
        if (!reducedBackward.insert(node).second)
            continue;

        for (auto edge : node->getInEdges())
        {
            const SVFGNode* pre = edge->getSrcNode();
            if (inUninitCandidateSlice(rawSlice, pre) && !shouldConsiderStoreForLoad(load, pre, rawSlice))
                backwardWorkList.push(pre);
        }
    }

    for (SVFGNodeSetIter it = reducedBackward.begin(), eit = reducedBackward.end(); it != eit; ++it)
        guardSlice->addToBackwardSlice(*it);

    return guardSlice;
}

bool UninitChecker::hasFeasibleUninitPath(ProgSlice* rawSlice, ProgSlice* guardSlice,
                                          const SVFGNode* load,
                                          GenericBug::EventStack& eventStack)
{
    const bool timeStat = Options::SaberTimeStat();
    double checkStart = 0;
    if (timeStat)
    {
        checkStart = SVFStat::getClk(true);
    }

    ProgSlice::VFWorkList worklist;
    worklist.push(rawSlice->getSource());
    guardSlice->setVFCond(rawSlice->getSource(), guardSlice->getTrueCond());

    u32_t visitedNodes = 0;
    while(!worklist.empty())
    {
        const SVFGNode* node = worklist.pop();
        ++visitedNodes;

        if (node == load)
        {
            ProgSlice::Condition loadCond = guardSlice->getVFCond(load);
            if (!guardSlice->isEquivalentBranchCond(loadCond, guardSlice->getFalseCond()) &&
                    getSaberCondAllocator()->isSatisfiable(loadCond))
            {
                guardSlice->setFinalCond(loadCond);
                guardSlice->evalFinalCond2Event(eventStack);
                if (const ICFGNode* loadICFG = getBugEventICFGNode(load))
                    eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, loadICFG));
                if (timeStat)
                {
                    double t = (SVFStat::getClk(true) - checkStart) / TIMEINTERVAL;
                    saberTimeStat.uninitLoadCheckTime += t;
                    if (t >= kSlowPathCheckSec)
                    {
                        outs() << "[UNINIT][path-check-slow] source=" << rawSlice->getSource()->getId()
                               << " load=" << load->getId()
                               << " visited=" << visitedNodes
                               << " time=" << t
                               << " guardBackward=" << guardSlice->getBackwardSliceSize()
                               << " hit=1\n";
                        outs().flush();
                    }
                }
                return true;
            }
        }

        guardSlice->setCurSVFGNode(node);
        ProgSlice::Condition invalidCond = guardSlice->computeInvalidCondFromRemovedSUVFEdge(node);
        ProgSlice::Condition cond = guardSlice->getVFCond(node);
        for(SVFGNode::const_iterator it = node->OutEdgeBegin(), eit = node->OutEdgeEnd(); it!=eit; ++it)
        {
            const SVFGEdge* edge = (*it);
            const SVFGNode* succ = edge->getDstNode();
            if(!guardSlice->inBackwardSlice(succ))
                continue;
            if (shouldConsiderStoreForLoad(load, succ, rawSlice))
                continue;

            ProgSlice::Condition vfCond;
            const SVFBasicBlock* nodeBB = guardSlice->getSVFGNodeBB(node);
            const SVFBasicBlock* succBB = guardSlice->getSVFGNodeBB(succ);
            if (nodeBB == nullptr || succBB == nullptr)
                continue;
            guardSlice->clearCFCond();

            if(edge->isCallVFGEdge())
            {
                const CallICFGNode* callSite = guardSlice->getCallSite(edge);
                if (callSite == nullptr || callSite->getParent() == nullptr)
                    continue;
                vfCond = guardSlice->ComputeInterCallVFGGuard(nodeBB, succBB, callSite->getParent());
            }
            else if(edge->isRetVFGEdge())
            {
                const CallICFGNode* retSite = guardSlice->getRetSite(edge);
                if (retSite == nullptr || retSite->getParent() == nullptr)
                    continue;
                vfCond = guardSlice->ComputeInterRetVFGGuard(nodeBB, succBB, retSite->getParent());
            }
            else
                vfCond = guardSlice->ComputeIntraVFGGuard(nodeBB, succBB);

            vfCond = guardSlice->condAnd(vfCond, guardSlice->condNeg(invalidCond));
            ProgSlice::Condition succPathCond = guardSlice->condAnd(cond, vfCond);
            if(guardSlice->setVFCond(succ, guardSlice->condOr(guardSlice->getVFCond(succ), succPathCond)))
                worklist.push(succ);
        }
    }

    if (timeStat)
    {
        double t = (SVFStat::getClk(true) - checkStart) / TIMEINTERVAL;
        saberTimeStat.uninitLoadCheckTime += t;
        if (t >= kSlowPathCheckSec)
        {
            outs() << "[UNINIT][path-check-slow] source=" << rawSlice->getSource()->getId()
                   << " load=" << load->getId()
                   << " visited=" << visitedNodes
                   << " time=" << t
                   << " guardBackward=" << guardSlice->getBackwardSliceSize()
                   << " hit=0\n";
            outs().flush();
        }
    }
    return false;
}


void UninitChecker::reportBug(ProgSlice* rawSlice)
{
    const bool timeStat = Options::SaberTimeStat();
    double reportStart = 0;
    if (timeStat)
    {
        reportStart = SVFStat::getClk(true);
        ++saberTimeStat.uninitReportCalls;
    }

    SVFGNodeSet qualifierStateIgnorePtrStore;
    SVFGNodeSet qualifierStateAllStore;

    double phaseStart = 0;
    if (timeStat)
    {
        phaseStart = SVFStat::getClk(true);
    }
    computeQualifierInferenceState(rawSlice, true, qualifierStateIgnorePtrStore);
    computeQualifierInferenceState(rawSlice, false, qualifierStateAllStore);
    if (timeStat)
    {
        double t = (SVFStat::getClk(true) - phaseStart) / TIMEINTERVAL;
        saberTimeStat.uninitQualifierTime += t;
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
    }
    auto finishReport = [&](u32_t candidateCount) {
        if (!timeStat)
            return;
        const double reportTime = (SVFStat::getClk(true) - reportStart) / TIMEINTERVAL;
        saberTimeStat.uninitReportTime += reportTime;
        if (reportTime >= kSlowSourceReportSec)
        {
            outs() << "[UNINIT][source-slow] source=" << rawSlice->getSource()->getId()
                   << " reportTime=" << reportTime
                   << " forwardSlice=" << rawSlice->getForwardSliceSize()
                   << " candidates=" << candidateCount
                   << "\n";
            outs().flush();
        }
    };

    if (candidateLoads.empty())
    {
        finishReport(0);
        return;
    }
    if (timeStat)
        ++saberTimeStat.uninitSourcesWithCandidates;

    GenericBug::EventStack eventStack;
    u32_t candidateIndex = 0;
    bool foundBug = false;
    for (SVFGNodeSetIter lit = candidateLoads.begin(), elit = candidateLoads.end(); lit != elit; ++lit)
    {
        ++candidateIndex;

        if (timeStat)
        {
            phaseStart = SVFStat::getClk(true);
        }
        std::unique_ptr<ProgSlice> guardSlice = buildStoreBypassGuardSlice(rawSlice, *lit);
        if (!guardSlice)
        {
            if (timeStat)
            {
                double t = (SVFStat::getClk(true) - phaseStart) / TIMEINTERVAL;
                saberTimeStat.uninitGuardBuildTime += t;
            }
            continue;
        }
        if (timeStat)
        {
            double t = (SVFStat::getClk(true) - phaseStart) / TIMEINTERVAL;
            saberTimeStat.uninitGuardBuildTime += t;
            if (guardSlice->getBackwardSliceSize() > saberTimeStat.uninitMaxGuardBackwardSlice)
                saberTimeStat.uninitMaxGuardBackwardSlice = guardSlice->getBackwardSliceSize();
        }

        double solveStart = 0;
        if (timeStat)
        {
            solveStart = SVFStat::getClk(true);
        }
        eventStack.clear();
        bool hasUninitPath = hasFeasibleUninitPath(rawSlice, guardSlice.get(), *lit, eventStack);
        if (timeStat)
        {
            double t = (SVFStat::getClk(true) - solveStart) / TIMEINTERVAL;
            addSolveTime(t);
            saberTimeStat.uninitGuardSolveTime += t;
        }

        if(hasUninitPath)
        {
            foundBug = true;
            break;
        }
    }

    if (foundBug)
    {
        const ICFGNode* sourceICFG = selectBestSourceICFGNode(rawSlice->getSource(), eventStack, getPAG());
        if (sourceICFG == nullptr)
        {
            finishReport(candidateLoads.size());
            return;
        }
        eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, sourceICFG));
        report.addSaberBug(GenericBug::UNINIT, eventStack);
        if (timeStat)
            ++saberTimeStat.uninitReportedSources;
    }
    finishReport(candidateLoads.size());
}

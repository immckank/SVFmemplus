// svf/lib/SABER/UninitChecker.cpp
#include "SABER/UninitChecker.h"
#include "SABER/SaberCheckerAPI.h"
#include "SABER/SaberMemTransferAPI.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFType.h"
#include "Util/SVFStat.h"
#include "Util/SVFUtil.h"
#include "Util/ExtAPI.h"
#include "SABER/ProgSlice.h"
#include "Util/WorkList.h"
#include "SVFIR/SVFValue.h"
#include "Graphs/ICFG.h"
#include "Util/cJSON.h"
#include <deque>
#include <queue>
#include <unordered_set>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>

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

UninitLLMTriageConfig UninitChecker::s_llmCfg;

void UninitChecker::setLLMTriageConfig(const UninitLLMTriageConfig& cfg)
{
    s_llmCfg = cfg;
}

static bool parseLocFileLine(const std::string& locJson, std::string& file, u32_t& line)
{
    file.clear();
    line = 0;
    cJSON* lj = cJSON_Parse(locJson.c_str());
    if (!lj)
        return false;
    if (cJSON* fl = cJSON_GetObjectItem(lj, "fl"))
        if (cJSON_IsString(fl) && fl->valuestring)
            file = fl->valuestring;
    if (cJSON* ln = cJSON_GetObjectItem(lj, "ln"))
        if (cJSON_IsNumber(ln))
            line = static_cast<u32_t>(ln->valuedouble);
    cJSON_Delete(lj);
    return !file.empty() || line != 0;
}

static const ICFGNode* getBugEventICFGNode(const SVFGNode* node)
{
    return node == nullptr ? nullptr : node->getICFGNode();
}

static bool isReturnAnchoredICFGNode(const ICFGNode* node)
{
    if (node == nullptr)
        return true;
    if (SVFUtil::isa<FunExitICFGNode>(node) || SVFUtil::isa<RetICFGNode>(node))
        return true;
    if (const IntraICFGNode* intra = SVFUtil::dyn_cast<IntraICFGNode>(node))
        return intra->isRetInst();
    return false;
}

static bool hasUsableSourceLoc(const ICFGNode* node)
{
    return node != nullptr && node->getSourceLoc().empty() == false &&
           !isReturnAnchoredICFGNode(node);
}

static const ICFGNode* findUseICFGNodeFromEvents(const GenericBug::EventStack& eventStack)
{
    for (auto it = eventStack.rbegin(); it != eventStack.rend(); ++it)
    {
        if (it->getEventType() == SVFBugEvent::Use && hasUsableSourceLoc(it->getEventInst()))
            return it->getEventInst();
    }
    return nullptr;
}

static int icfgNodeKindPenalty(const ICFGNode* node)
{
    if (node == nullptr)
        return 100;

    // Smaller penalty means "better" for a user-facing anchor location.
    if (SVFUtil::isa<IntraICFGNode>(node))
        return 0;
    if (SVFUtil::isa<CallICFGNode>(node))
        return 2;
    if (SVFUtil::isa<RetICFGNode>(node) || SVFUtil::isa<FunExitICFGNode>(node))
        return 100;
    if (SVFUtil::isa<FunEntryICFGNode>(node))
        return 5;
    if (const IntraICFGNode* intra = SVFUtil::dyn_cast<IntraICFGNode>(node))
    {
        if (intra->isRetInst())
            return 100;
    }
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

    // Stack alloca sites are the preferred anchor when nearby search would land on returns.
    if (const AddrSVFGNode* addr = SVFUtil::dyn_cast<AddrSVFGNode>(source))
    {
        if (const SVFVar* obj = SVFUtil::dyn_cast<SVFVar>(addr->getPAGSrcNode()))
        {
            if (const StackObjVar* stackObj = SVFUtil::dyn_cast<StackObjVar>(obj))
            {
                if (const ICFGNode* allocICFG = stackObj->getICFGNode())
                {
                    if (!isReturnAnchoredICFGNode(allocICFG) && allocICFG->getSourceLoc().empty() == false)
                        return allocICFG;
                }
            }
        }
    }

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
    llmTriage.setConfig(s_llmCfg);
}
UninitChecker::~UninitChecker() { }

bool UninitChecker::runOnModule(SVFIR* pag) {
    analyze();
    return false;
}

void UninitChecker::analyze()
{
    const bool progress = Options::UninitCheck();
    const bool timeStat = Options::SaberTimeStat();
    double totalStart = 0;
    double nextProgressTime = 0;
    if (progress || timeStat)
    {
        totalStart = SVFStat::getClk(true);
        nextProgressTime = totalStart + 5 * TIMEINTERVAL;
    }

    initialize();

    reportedKeys.clear();
    pendingReports.clear();
    dedupSkippedReports = 0;
    headerSkippedReports = 0;
    llmSuppressedReports = 0;
    emittedUninitReports = 0;
    llmTriage = UninitLLMTriage();
    llmTriage.setConfig(s_llmCfg);

    ContextCond::setMaxCxtLen(Options::CxtLimit());

    const u32_t numSrcs = getSources().size();
    if (progress)
    {
        outs() << "[UNINIT][analyze-begin] sources=" << numSrcs
               << " criticalSinks=" << criticalSinkNodes.size()
               << " allLoads=" << loadNodes.size()
               << " smallInitSkipped=" << smallInitSkippedSources << "\n";
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
        if (progress)
        {
            const double now = SVFStat::getClk(true);
            if (now >= nextProgressTime || sourceIndex == numSrcs)
            {
                outs() << "[UNINIT][source-progress] index=" << sourceIndex
                       << "/" << numSrcs
                       << " elapsed=" << (now - totalStart) / TIMEINTERVAL
                       << " withCandidates=" << saberTimeStat.uninitSourcesWithCandidates
                       << " reported=" << saberTimeStat.uninitReportedSources
                       << "\n";
                outs().flush();
                nextProgressTime = now + 5 * TIMEINTERVAL;
            }
        }
    }

    if (progress || timeStat)
    {
        if (timeStat)
            saberTimeStat.totalTime = (SVFStat::getClk(true) - totalStart) / TIMEINTERVAL;
    }
    finalize();
    if (progress || timeStat)
    {
        const double elapsed = (SVFStat::getClk(true) - totalStart) / TIMEINTERVAL;
        if (progress)
        {
            outs() << "[UNINIT][analyze-done] sources=" << numSrcs
                   << " withCandidates=" << saberTimeStat.uninitSourcesWithCandidates
                   << " pending=" << pendingReports.size()
                   << " reported=" << emittedUninitReports
                   << " smallInitSkipped=" << smallInitSkippedSources
                   << " elapsed=" << elapsed << "\n";
            outs().flush();
        }
    }
}

bool UninitChecker::isParameterSpillStackObject(StackObjVar* stackObj, SVFIR* pag) const
{
    const FunObjVar* fun = stackObj->getFunction();
    if (fun == nullptr)
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

bool UninitChecker::isSmallDirectlyInitializedStackObject(StackObjVar* stackObj, SVFIR* pag) const
{
    if (stackObj == nullptr || pag == nullptr)
        return false;
    if (!stackObj->isConstantByteSize())
        return false;
    const u32_t byteSize = stackObj->getByteSizeOfObj();
    if (byteSize == 0 || byteSize > 16)
        return false;
    if (!stackObj->hasOutgoingEdges(SVFStmt::Addr))
        return false;

    bool hasDirectInitStore = false;
    for (const SVFStmt* addrStmt : stackObj->getOutgoingEdges(SVFStmt::Addr))
    {
        const SVFVar* slotPtr = addrStmt->getDstNode();
        if (slotPtr == nullptr || !slotPtr->hasIncomingEdges(SVFStmt::Store))
            continue;

        for (SVFStmt::SVFStmtSetTy::const_iterator sit = slotPtr->getIncomingEdgesBegin(SVFStmt::Store),
                seit = slotPtr->getIncomingEdgesEnd(SVFStmt::Store); sit != seit; ++sit)
        {
            const StoreStmt* store = SVFUtil::dyn_cast<StoreStmt>(*sit);
            if (store == nullptr)
                continue;

            const SVFVar* rhs = store->getRHSVar();
            if (rhs == nullptr)
                continue;
            if (rhs->isPointer())
            {
                RegionSet rhsRegions;
                if (addPointeeRegions(rhs->getId(), rhsRegions))
                {
                    RegionSet objRegions;
                    if (addObjectRegion(stackObj->getId(), objRegions) &&
                            regionSetsMayIntersect(rhsRegions, objRegions))
                        continue;
                }
            }
            hasDirectInitStore = true;
        }
    }

    return hasDirectInitStore;
}

bool UninitChecker::isReturnAnchoredICFGNode(const ICFGNode* node) const
{
    return ::isReturnAnchoredICFGNode(node);
}

bool UninitChecker::isCompositeMemoryObject(const BaseObjVar* obj) const
{
    if (obj == nullptr)
        return false;

    // ObjTypeInfo flags are set in SymbolTableBuilder::analyzeObjType from LLVM
    // StructType / ArrayType. Scalars and vector SingleValueTypes are excluded.
    return obj->isStruct() || obj->isVarStruct() || obj->isArray() || obj->isVarArray();
}

bool UninitChecker::isSmallScalarStackObject(const BaseObjVar* obj) const
{
    const StackObjVar* stackObj = SVFUtil::dyn_cast<StackObjVar>(obj);
    if (stackObj == nullptr || isCompositeMemoryObject(stackObj))
        return false;
    if (!stackObj->isConstantByteSize())
        return false;
    const u32_t byteSize = stackObj->getByteSizeOfObj();
    return byteSize > 0 && byteSize <= 16;
}

bool UninitChecker::pagNodeMayReachCallPE(const SVFVar* node, SVFIR* pag) const
{
    if (node == nullptr || pag == nullptr)
        return false;

    FIFOWorkList<const SVFVar*> worklist;
    std::unordered_set<const SVFVar*> visited;
    worklist.push(node);

    while (!worklist.empty())
    {
        const SVFVar* cur = worklist.pop();
        if (!visited.insert(cur).second)
            continue;

        SVFVar* mutableCur = const_cast<SVFVar*>(cur);
        if (mutableCur->hasOutgoingEdges(SVFStmt::Call))
            return true;

        for (const SVFStmt* stmt : mutableCur->getOutgoingEdges(SVFStmt::Copy))
            worklist.push(stmt->getDstNode());
        for (const SVFStmt* stmt : mutableCur->getOutgoingEdges(SVFStmt::Gep))
            worklist.push(stmt->getDstNode());
        for (const SVFStmt* stmt : mutableCur->getOutgoingEdges(SVFStmt::Load))
            worklist.push(stmt->getDstNode());
        for (const SVFStmt* stmt : mutableCur->getOutgoingEdges(SVFStmt::Store))
            worklist.push(stmt->getSrcNode());
    }
    return false;
}

bool UninitChecker::isAddressEscapingScalarStackObject(StackObjVar* stackObj, SVFIR* pag) const
{
    if (stackObj == nullptr || pag == nullptr)
        return false;
    if (!stackObj->hasOutgoingEdges(SVFStmt::Addr))
        return false;

    for (const SVFStmt* addrStmt : stackObj->getOutgoingEdges(SVFStmt::Addr))
    {
        const SVFVar* slotPtr = addrStmt->getDstNode();
        if (slotPtr != nullptr && pagNodeMayReachCallPE(slotPtr, pag))
            return true;
    }
    return false;
}

bool UninitChecker::isTrackableScalarStackObject(StackObjVar* stackObj, SVFIR* pag) const
{
    if (!isSmallScalarStackObject(stackObj))
        return false;
    return isAddressEscapingScalarStackObject(stackObj, pag);
}

bool UninitChecker::isScalarStackValueLoad(const SVFGNode* load) const
{
    if (!SVFUtil::isa<LoadSVFGNode>(load) || isPtrLoadNode(load))
        return false;

    const RegionSet& readRegions = getLoadReadRegions(load);
    for (const RegionKey& region : readRegions)
    {
        const BaseObjVar* baseObj = getPAG()->getBaseObject(region.base);
        if (isSmallScalarStackObject(baseObj))
            return true;
    }
    return false;
}

bool UninitChecker::pointeeIncludesCompositeObject(NodeID ptr) const
{
    PointerAnalysis* pta = getSVFG()->getPTA();
    if (pta == nullptr)
        return false;

    const PointsTo& pts = pta->getPts(ptr);
    for (PointsTo::iterator it = pts.begin(), eit = pts.end(); it != eit; ++it)
    {
        const BaseObjVar* baseObj = getPAG()->getBaseObject(*it);
        if (isCompositeMemoryObject(baseObj))
            return true;
    }
    return false;
}

UninitChecker::RegionKey UninitChecker::makeWholeRegion(NodeID obj) const
{
    const BaseObjVar* baseObj = getPAG()->getBaseObject(obj);
    return RegionKey(baseObj ? baseObj->getId() : obj, 0, RegionKey::WholeObject);
}

UninitChecker::RegionKey UninitChecker::makeFieldRegion(NodeID obj, APOffset field) const
{
    const BaseObjVar* baseObj = getPAG()->getBaseObject(obj);
    return RegionKey(baseObj ? baseObj->getId() : obj, field, RegionKey::Field);
}

UninitChecker::RegionKey UninitChecker::makeCollapsedRegion(NodeID obj) const
{
    const BaseObjVar* baseObj = getPAG()->getBaseObject(obj);
    return RegionKey(baseObj ? baseObj->getId() : obj, 0, RegionKey::CollapsedObject);
}

UninitChecker::RegionKey UninitChecker::makeUnknownRegion(NodeID obj) const
{
    const BaseObjVar* baseObj = getPAG()->getBaseObject(obj);
    return RegionKey(baseObj ? baseObj->getId() : obj, 0, RegionKey::Unknown);
}

bool UninitChecker::regionsMayIntersect(const RegionKey& lhs, const RegionKey& rhs) const
{
    if (lhs.base != rhs.base)
        return false;

    if (lhs.precision == RegionKey::Unknown || rhs.precision == RegionKey::Unknown)
        return true;
    if (lhs.precision == RegionKey::CollapsedObject || rhs.precision == RegionKey::CollapsedObject)
        return true;
    if (lhs.precision == RegionKey::WholeObject || rhs.precision == RegionKey::WholeObject)
        return true;

    return lhs.field == rhs.field;
}

bool UninitChecker::regionSetsMayIntersect(const RegionSet& lhs, const RegionSet& rhs) const
{
    for (const RegionKey& left : lhs)
    {
        for (const RegionKey& right : rhs)
        {
            if (regionsMayIntersect(left, right))
                return true;
        }
    }
    return false;
}

bool UninitChecker::regionCoveredByWrite(const RegionKey& region, const RegionKey& writeRegion) const
{
    if (region.base != writeRegion.base)
        return false;

    if (writeRegion.precision == RegionKey::Unknown)
        return region.precision == RegionKey::Unknown;

    if (writeRegion.precision == RegionKey::WholeObject)
        return true;

    if (writeRegion.precision == RegionKey::CollapsedObject)
        return region.precision == RegionKey::CollapsedObject;

    if (region.precision == RegionKey::Field)
        return region.field == writeRegion.field;

    // A field write does not prove that an unknown/whole-object region is fully initialized.
    return false;
}

bool UninitChecker::regionSetsCover(const RegionSet& regions, const RegionSet& writeRegions) const
{
    if (regions.empty() || writeRegions.empty())
        return false;

    for (const RegionKey& region : regions)
    {
        bool covered = false;
        for (const RegionKey& writeRegion : writeRegions)
        {
            if (regionCoveredByWrite(region, writeRegion))
            {
                covered = true;
                break;
            }
        }
        if (!covered)
            return false;
    }
    return true;
}

bool UninitChecker::eraseInitializedRegions(RegionSet& state, const RegionSet& writeRegions) const
{
    bool changed = false;
    for (RegionSet::iterator it = state.begin(); it != state.end();)
    {
        bool covered = false;
        for (const RegionKey& writeRegion : writeRegions)
        {
            if (regionCoveredByWrite(*it, writeRegion))
            {
                covered = true;
                break;
            }
        }

        if (covered)
        {
            it = state.erase(it);
            changed = true;
        }
        else
            ++it;
    }
    return changed;
}

bool UninitChecker::getStoreStmtWriteRegions(const StoreStmt* store, RegionSet& regions) const
{
    regions.clear();
    if (store == nullptr || store->getLHSVar() == nullptr)
        return false;
    return addPointeeRegions(store->getLHSVarID(), regions);
}

bool UninitChecker::storeStmtRHSMayBeUninitSource(const StoreStmt* store, const SVFGNode* source) const
{
    if (store == nullptr || source == nullptr)
        return true;

    const SVFVar* rhs = store->getRHSVar();
    if (rhs == nullptr)
        return true;
    if (rhs->isConstDataOrAggDataButNotNullPtr())
        return false;

    const SVFGNode* rhsDef = getSVFG()->getDefSVFGNode(rhs);
    if (rhsDef == source)
        return true;

    if (rhs->isPointer())
    {
        RegionSet rhsRegions;
        RegionSet sourceRegions;
        if (addPointeeRegions(rhs->getId(), rhsRegions) &&
                getInitialRegionsForSource(source, sourceRegions) &&
                regionSetsMayIntersect(rhsRegions, sourceRegions))
            return true;
    }

    return false;
}

bool UninitChecker::isMustExecuteDirectInitStore(const StoreStmt* store, const BaseObjVar* obj) const
{
    if (store == nullptr || obj == nullptr)
        return false;

    const ICFGNode* storeNode = store->getICFGNode();
    const ICFGNode* objNode = obj->getICFGNode();
    if (storeNode == nullptr || objNode == nullptr)
        return false;

    const FunObjVar* fun = objNode->getFun();
    if (fun == nullptr || storeNode->getFun() != fun)
        return false;

    const SVFBasicBlock* storeBB = storeNode->getBB();
    if (storeBB == nullptr)
        return false;

    const auto& domTree = fun->getDomTreeMap();
    if (domTree.empty())
        return false;

    for (auto it = domTree.begin(), eit = domTree.end(); it != eit; ++it)
    {
        const SVFBasicBlock* bb = it->first;
        if (bb != storeBB && !fun->dominate(storeBB, bb))
            return false;
    }
    return true;
}

bool UninitChecker::refineInitialRegionsWithDirectStores(const BaseObjVar* obj, const SVFGNode* source,
                                                         RegionSet& regions) const
{
    if (obj == nullptr || source == nullptr || regions.empty())
        return false;

    bool changed = false;
    std::deque<SVFVar*> worklist;
    std::unordered_set<const SVFVar*> visited;

    if (SVFVar* objVar = const_cast<SVFVar*>(SVFUtil::dyn_cast<SVFVar>(obj)))
    {
        if (objVar->hasOutgoingEdges(SVFStmt::Addr))
        {
            for (const SVFStmt* addrStmt : objVar->getOutgoingEdges(SVFStmt::Addr))
            {
                if (addrStmt != nullptr && addrStmt->getDstNode() != nullptr)
                    worklist.push_back(addrStmt->getDstNode());
            }
        }
    }

    u32_t steps = 0;
    constexpr u32_t kMaxDirectInitPtrTrace = 64;
    while (!worklist.empty() && steps++ < kMaxDirectInitPtrTrace)
    {
        SVFVar* ptr = worklist.front();
        worklist.pop_front();
        if (ptr == nullptr || !visited.insert(ptr).second)
            continue;

        if (ptr->hasIncomingEdges(SVFStmt::Store))
        {
            for (SVFStmt::SVFStmtSetTy::const_iterator sit = ptr->getIncomingEdgesBegin(SVFStmt::Store),
                    seit = ptr->getIncomingEdgesEnd(SVFStmt::Store); sit != seit; ++sit)
            {
                const StoreStmt* store = SVFUtil::dyn_cast<StoreStmt>(*sit);
                if (store == nullptr)
                    continue;

                RegionSet writeRegions;
                if (!getStoreStmtWriteRegions(store, writeRegions))
                    continue;
                if (!regionSetsMayIntersect(regions, writeRegions))
                    continue;
                if (storeStmtRHSMayBeUninitSource(store, source))
                    continue;
                if (!isMustExecuteDirectInitStore(store, obj))
                    continue;

                changed |= eraseInitializedRegions(regions, writeRegions);
                if (regions.empty())
                    return changed;
            }
        }

        if (ptr->hasOutgoingEdges(SVFStmt::Gep))
        {
            for (const SVFStmt* gepStmt : ptr->getOutgoingEdges(SVFStmt::Gep))
            {
                if (gepStmt != nullptr && gepStmt->getDstNode() != nullptr)
                    worklist.push_back(gepStmt->getDstNode());
            }
        }
        if (ptr->hasOutgoingEdges(SVFStmt::Copy))
        {
            for (const SVFStmt* copyStmt : ptr->getOutgoingEdges(SVFStmt::Copy))
            {
                if (copyStmt != nullptr && copyStmt->getDstNode() != nullptr)
                    worklist.push_back(copyStmt->getDstNode());
            }
        }
    }
    return changed;
}

bool UninitChecker::mergeRegionState(NodeToRegionStateMap& states, const SVFGNode* node,
                                     const RegionSet& incoming) const
{
    if (node == nullptr || incoming.empty())
        return false;

    RegionSet& state = states[node];
    bool changed = false;
    for (const RegionKey& region : incoming)
    {
        if (state.insert(region).second)
            changed = true;
    }
    return changed;
}

bool UninitChecker::storeRHSMayCarryUninitInState(const SVFGNode* store, ProgSlice* slice,
                                                 const NodeToRegionStateMap& states) const
{
    const StoreSVFGNode* storeNode = SVFUtil::dyn_cast<StoreSVFGNode>(store);
    if (storeNode == nullptr)
        return true;

    const SVFVar* rhs = storeNode->getPAGSrcNode();
    if (rhs == nullptr)
        return true;

    if (rhs->isConstDataOrAggDataButNotNullPtr())
        return false;

    const SVFGNode* rhsDef = getSVFG()->getDefSVFGNode(rhs);
    if (rhsDef == nullptr)
        return true;

    if (storeRHSMayCarryUninit(store, slice))
        return true;

    // Address constants and freshly materialized values do not carry bytes from
    // the current uninitialized object. For value-producing nodes, consult the
    // current source-local region state instead of the whole forward slice.
    if (!(SVFUtil::isa<LoadSVFGNode>(rhsDef) ||
          SVFUtil::isa<CopySVFGNode>(rhsDef) ||
          SVFUtil::isa<PHISVFGNode>(rhsDef) ||
          SVFUtil::isa<ActualRetVFGNode>(rhsDef) ||
          SVFUtil::isa<FormalParmVFGNode>(rhsDef)))
        return false;

    NodeToRegionStateMap::const_iterator stateIt = states.find(rhsDef);
    return stateIt != states.end() && !stateIt->second.empty();
}

void UninitChecker::computeRegionUninitState(ProgSlice* slice, NodeToRegionStateMap& states) const
{
    states.clear();
    if (slice == nullptr || slice->getSource() == nullptr)
        return;

    RegionSet sourceRegions;
    if (!getInitialRegionsForSource(slice->getSource(), sourceRegions))
        return;

    std::deque<const SVFGNode*> worklist;
    if (mergeRegionState(states, slice->getSource(), sourceRegions))
        worklist.push_back(slice->getSource());

    while (!worklist.empty())
    {
        const SVFGNode* node = worklist.front();
        worklist.pop_front();

        NodeToRegionStateMap::const_iterator stateIt = states.find(node);
        if (stateIt == states.end() || stateIt->second.empty())
            continue;

        RegionSet outgoingState = stateIt->second;
        if (storeNodes.find(node) != storeNodes.end() && isStoreStrongRegionKill(node) &&
                !storeRHSMayCarryUninitInState(node, slice, states))
        {
            const RegionSet& writeRegions = getStoreWriteRegions(node);
            eraseInitializedRegions(outgoingState, writeRegions);
        }

        if (outgoingState.empty())
            continue;

        for (auto edge : node->getOutEdges())
        {
            const SVFGNode* succ = edge->getDstNode();
            if (!inUninitCandidateSlice(slice, succ))
                continue;
            if (mergeRegionState(states, succ, outgoingState))
                worklist.push_back(succ);
        }
    }
}

bool UninitChecker::loadMayReadUninitRegion(const SVFGNode* load, const NodeToRegionStateMap& states) const
{
    NodeToRegionStateMap::const_iterator stateIt = states.find(load);
    if (stateIt == states.end() || stateIt->second.empty())
        return false;

    const RegionSet& readRegions = getLoadReadRegions(load);
    return !readRegions.empty() && regionSetsMayIntersect(stateIt->second, readRegions);
}

bool UninitChecker::addObjectRegion(NodeID obj, RegionSet& regions) const
{
    SVFIR* pag = getPAG();
    const SVFVar* var = pag->getGNode(obj);
    const BaseObjVar* baseObj = pag->getBaseObject(obj);
    if (baseObj == nullptr)
        return false;

    if (baseObj->isFieldInsensitive() || baseObj->isArray() || baseObj->isVarArray())
    {
        regions.insert(makeCollapsedRegion(baseObj->getId()));
        return true;
    }

    if (const GepObjVar* gepObj = SVFUtil::dyn_cast<GepObjVar>(var))
    {
        regions.insert(makeFieldRegion(gepObj->getBaseNode(), gepObj->getConstantFieldIdx()));
        return true;
    }

    bool addedField = false;
    const NodeBS& fields = pag->getAllFieldsObjVars(baseObj->getId());
    for (NodeBS::iterator fit = fields.begin(), feit = fields.end(); fit != feit; ++fit)
    {
        if (*fit == baseObj->getId())
            continue;
        if (const GepObjVar* fieldObj = SVFUtil::dyn_cast<GepObjVar>(pag->getGNode(*fit)))
        {
            regions.insert(makeFieldRegion(fieldObj->getBaseNode(), fieldObj->getConstantFieldIdx()));
            addedField = true;
        }
    }

    if (!addedField)
        regions.insert(makeWholeRegion(baseObj->getId()));
    return true;
}

bool UninitChecker::addPointeeRegions(NodeID ptr, RegionSet& regions) const
{
    PointerAnalysis* pta = getSVFG()->getPTA();
    if (pta == nullptr)
        return false;

    bool added = false;
    const PointsTo& pts = pta->getPts(ptr);
    for (PointsTo::iterator it = pts.begin(), eit = pts.end(); it != eit; ++it)
        added |= addObjectRegion(*it, regions);
    return added;
}

bool UninitChecker::getInitialRegionsForSource(const SVFGNode* source, RegionSet& regions) const
{
    regions.clear();
    auto it = sourceInitialRegions.find(source);
    if (it != sourceInitialRegions.end())
    {
        regions = it->second;
        return !regions.empty();
    }

    if (const AddrSVFGNode* addr = SVFUtil::dyn_cast<AddrSVFGNode>(source))
        return addObjectRegion(addr->getPAGSrcNodeID(), regions);

    if (const StmtSVFGNode* stmt = SVFUtil::dyn_cast<StmtSVFGNode>(source))
        return addPointeeRegions(stmt->getPAGDstNodeID(), regions);

    return false;
}

const UninitChecker::RegionSet& UninitChecker::getLoadReadRegions(const SVFGNode* load) const
{
    auto cached = loadReadRegionCache.find(load);
    if (cached != loadReadRegionCache.end())
        return cached->second;

    RegionSet regions;
    if (const LoadSVFGNode* loadNode = SVFUtil::dyn_cast<LoadSVFGNode>(load))
        addPointeeRegions(loadNode->getPAGSrcNodeID(), regions);

    loadReadRegionCache[load] = regions;
    return loadReadRegionCache[load];
}

const UninitChecker::RegionSet& UninitChecker::getStoreWriteRegions(const SVFGNode* store) const
{
    auto cached = storeWriteRegionCache.find(store);
    if (cached != storeWriteRegionCache.end())
        return cached->second;

    RegionSet regions;
    if (const StoreSVFGNode* storeNode = SVFUtil::dyn_cast<StoreSVFGNode>(store))
        addPointeeRegions(storeNode->getPAGDstNodeID(), regions);

    storeWriteRegionCache[store] = regions;
    return storeWriteRegionCache[store];
}

bool UninitChecker::isMemsetLikeInitializingStore(const SVFGNode* store) const
{
    const StoreSVFGNode* storeNode = SVFUtil::dyn_cast<StoreSVFGNode>(store);
    if (storeNode == nullptr)
        return false;

    const ICFGNode* icfg = storeNode->getICFGNode();
    if (icfg == nullptr)
        return false;

    if (const CallICFGNode* call = SVFUtil::dyn_cast<CallICFGNode>(icfg))
    {
        CallGraph::FunctionSet callees;
        getCallgraph()->getCallees(call, callees);
        for (const FunObjVar* fun : callees)
        {
            if (fun == nullptr)
                continue;
            const std::string& n = fun->getName();
            if (ExtAPI::getExtAPI()->is_memset(fun) || n == "bzero" || n == "memzero_explicit" ||
                n.find("memset") != std::string::npos)
                return true;
        }
    }

    const SVFVar* rhs = storeNode->getPAGSrcNode();
    if (rhs != nullptr && rhs->isConstDataOrAggDataButNotNullPtr())
        return true;

    return false;
}

bool UninitChecker::isStoreStrongRegionKill(const SVFGNode* store) const
{
    return classifyStrongUpdateFailure(store) == StrongOK;
}

UninitChecker::StrongUpdateFailureReason UninitChecker::classifyStrongUpdateFailure(const SVFGNode* store) const
{
    const StoreSVFGNode* storeNode = SVFUtil::dyn_cast<StoreSVFGNode>(store);
    if (storeNode == nullptr)
        return StrongNotStore;

    PointerAnalysis* pta = getSVFG()->getPTA();
    if (pta == nullptr)
        return StrongNoPTA;

    const PointsTo& pts = pta->getPts(storeNode->getPAGDstNodeID());
    if (pts.count() != 1)
        return StrongMultiPts;

    const RegionSet& writeRegions = getStoreWriteRegions(store);
    if (writeRegions.size() != 1)
        return StrongMultiRegion;

    const RegionKey& writeRegion = *writeRegions.begin();
    if (writeRegion.precision == RegionKey::CollapsedObject)
        return StrongCollapsedRegion;
    if (writeRegion.precision == RegionKey::Unknown)
        return StrongUnknownRegion;

    const BaseObjVar* baseObj = getPAG()->getBaseObject(writeRegion.base);
    if (baseObj == nullptr || baseObj->isFieldInsensitive())
        return StrongFieldInsensitive;
    if (baseObj->isVarArray())
        return StrongVarArray;

    return StrongOK;
}

const char* UninitChecker::strongUpdateFailureName(StrongUpdateFailureReason reason) const
{
    switch (reason)
    {
    case StrongOK:
        return "strong-ok";
    case StrongNotStore:
        return "not-store";
    case StrongNoPTA:
        return "no-pta";
    case StrongMultiPts:
        return "multi-pts";
    case StrongMultiRegion:
        return "multi-region";
    case StrongCollapsedRegion:
        return "collapsed-region";
    case StrongUnknownRegion:
        return "unknown-region";
    case StrongFieldInsensitive:
        return "field-insensitive";
    case StrongVarArray:
        return "var-array";
    }
    return "unknown";
}

bool UninitChecker::storeMayKillLoadRegion(const SVFGNode* store, const SVFGNode* load) const
{
    if (storeNodes.find(store) == storeNodes.end())
        return false;

    if (isMemsetLikeInitializingStore(store))
    {
        const RegionSet& writeRegions = getStoreWriteRegions(store);
        const RegionSet& readRegions = getLoadReadRegions(load);
        if (!writeRegions.empty() && !readRegions.empty() &&
                regionSetsMayIntersect(writeRegions, readRegions))
            return true;
        return false;
    }

    if (!isStoreStrongRegionKill(store))
        return false;

    const RegionSet& writeRegions = getStoreWriteRegions(store);
    const RegionSet& readRegions = getLoadReadRegions(load);
    if (writeRegions.empty() || readRegions.empty())
        return false;

    return regionSetsMayIntersect(writeRegions, readRegions);
}

bool UninitChecker::isSameAddressStoreLoad(const SVFGNode* store, const SVFGNode* load) const
{
    const StoreSVFGNode* storeNode = SVFUtil::dyn_cast<StoreSVFGNode>(store);
    const LoadSVFGNode* loadNode = SVFUtil::dyn_cast<LoadSVFGNode>(load);
    if (storeNode == nullptr || loadNode == nullptr)
        return false;

    return storeNode->getPAGDstNodeID() == loadNode->getPAGSrcNodeID();
}

bool UninitChecker::isLoadSpecificStoreKill(const SVFGNode* load, const SVFGNode* store, ProgSlice* slice) const
{
    if (storeNodes.find(store) == storeNodes.end())
        return false;
    if (!isSameAddressStoreLoad(store, load))
        return false;

    const RegionSet& writeRegions = getStoreWriteRegions(store);
    const RegionSet& readRegions = getLoadReadRegions(load);
    if (!regionSetsCover(readRegions, writeRegions))
        return false;

    return !storeRHSMayCarryUninit(store, slice);
}

bool UninitChecker::storeRHSMayCarryUninit(const SVFGNode* store, ProgSlice* slice) const
{
    const StoreSVFGNode* storeNode = SVFUtil::dyn_cast<StoreSVFGNode>(store);
    if (storeNode == nullptr || slice == nullptr)
        return false;

    const SVFVar* rhs = storeNode->getPAGSrcNode();
    if (rhs == nullptr || rhs->isConstDataOrAggDataButNotNullPtr())
        return false;

    const SVFGNode* rhsDef = getSVFG()->getDefSVFGNode(storeNode->getPAGSrcNode());
    if (rhsDef == nullptr || !slice->inForwardSlice(rhsDef))
        return false;

    return SVFUtil::isa<LoadSVFGNode>(rhsDef) ||
           SVFUtil::isa<CopySVFGNode>(rhsDef) ||
           SVFUtil::isa<PHISVFGNode>(rhsDef) ||
           SVFUtil::isa<ActualRetVFGNode>(rhsDef) ||
           SVFUtil::isa<FormalParmVFGNode>(rhsDef);
}

bool UninitChecker::isZeroingAllocatorName(const std::string& name) const
{
    if (name.find("calloc") != std::string::npos ||
        name.find("zalloc") != std::string::npos ||
        name.find("alloc_clear") != std::string::npos ||
        name.find("alloc_zero") != std::string::npos ||
        name == "vzalloc" ||
        name == "kvzalloc" ||
        name == "kzalloc" ||
        name == "devm_kzalloc" ||
        name == "kcalloc" ||
        name.find("kzalloc_node") != std::string::npos ||
        name.find("devm_kzalloc") != std::string::npos)
        return true;
    return false;
}

bool UninitChecker::isPlainKmallocAllocatorName(const std::string& name) const
{
    if (isZeroingAllocatorName(name))
        return false;
    if (name == "kmalloc" || name == "__kmalloc" || name == "kmalloc_node" ||
        name == "kmalloc_array" || name == "kmem_cache_alloc")
        return true;
    if (name.find("devm_kmalloc") != std::string::npos)
        return true;
    if (name.find("kmalloc") != std::string::npos)
        return true;
    if (name.find("kmem_cache_alloc") != std::string::npos)
        return true;
    return false;
}

bool UninitChecker::isZeroingHeapObject(const HeapObjVar* heapObj) const
{
    if (heapObj == nullptr)
        return false;

    const ICFGNode* icfgNode = heapObj->getICFGNode();
    const CallICFGNode* call = SVFUtil::dyn_cast<CallICFGNode>(icfgNode);
    if (call == nullptr)
        return false;

    CallGraph::FunctionSet callees;
    getCallgraph()->getCallees(call, callees);
    for (CallGraph::FunctionSet::const_iterator it = callees.begin(), eit = callees.end(); it != eit; ++it)
    {
        const FunObjVar* fun = *it;
        if (fun != nullptr && SaberCheckerAPI::getCheckerAPI()->isMemAlloc(fun) &&
                isZeroingAllocatorName(fun->getName()))
            return true;
    }
    return false;
}

void UninitChecker::initSrcs()
{
    SVFIR* pag = getPAG();

    summaryBoundaryToLoads.clear();
    summaryBoundaryToBoundaries.clear();
    ignorePtrStoreForLoadCache.clear();
    sourceInitialRegions.clear();
    loadReadRegionCache.clear();
    storeWriteRegionCache.clear();
    smallInitSkippedSources = 0;

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
                    const bool composite = isCompositeMemoryObject(stackObj);
                    const bool scalarOutParam = isTrackableScalarStackObject(stackObj, pag);
                    if (!composite && !scalarOutParam)
                        continue;
                    if (isParameterSpillStackObject(stackObj, pag))
                        continue;
                    if (isSmallDirectlyInitializedStackObject(stackObj, pag))
                    {
                        ++smallInitSkippedSources;
                        continue;
                    }
                    if (isReturnAnchoredICFGNode(stackObj->getICFGNode()))
                        continue;
                    const SVFGNode* source = getSVFG()->getStmtVFGNode(ld);
                    RegionSet initialRegions;
                    if (!addObjectRegion(stackObj->getId(), initialRegions))
                        continue;
                    refineInitialRegionsWithDirectStores(stackObj, source, initialRegions);
                    if (initialRegions.empty())
                    {
                        ++smallInitSkippedSources;
                        continue;
                    }
                    sourceInitialRegions[source] = initialRegions;
                    addToSources(source);
                }
                else if (HeapObjVar* heapObj = SVFUtil::dyn_cast<HeapObjVar>(obj))
                {
                    // B2: direct composite HeapObjVar sources are dominated by kmalloc FP;
                    // heap uninit is tracked only via explicit non-kmalloc call-site rets.
                    (void)heapObj;
                    continue;
                }
            }
        }
    }

    for(SVFIR::CSToRetMap::iterator it = pag->getCallSiteRets().begin(),
            eit = pag->getCallSiteRets().end(); it != eit; ++it)
    {
        const RetICFGNode* ret = it->first;
        if((!Options::RunUncallFuncs() && ret->getFun()->isUncalledFunction()) || !ret->getType()->isPointerTy())
            continue;

        CallGraph::FunctionSet callees;
        getCallgraph()->getCallees(ret->getCallICFGNode(), callees);
        for(CallGraph::FunctionSet::const_iterator cit = callees.begin(), ecit = callees.end(); cit != ecit; ++cit)
        {
            const FunObjVar* fun = *cit;
            if (!SaberCheckerAPI::getCheckerAPI()->isMemAlloc(fun) || isZeroingAllocatorName(fun->getName()))
                continue;
            // B2: skip plain kmalloc-family allocators (major FP source in kernel drivers).
            if (isPlainKmallocAllocatorName(fun->getName()))
                continue;

            CSWorkList worklist;
            SVFGNodeBS visited;
            worklist.push(ret->getCallICFGNode());
            while (!worklist.empty())
            {
                const CallICFGNode* cs = worklist.pop();
                const RetICFGNode* retBlockNode = cs->getRetICFGNode();
                const PAGNode* pagNode = pag->getCallSiteRet(retBlockNode);
                if (pagNode == nullptr)
                    continue;

                const SVFGNode* source = getSVFG()->getDefSVFGNode(pagNode);
                if (visited.test(source->getId()) == 0)
                    visited.set(source->getId());
                else
                    continue;

                CallSiteSet csSet;
                if (isInAWrapper(source, csSet))
                {
                    for (CallSiteSet::iterator csIt = csSet.begin(), csEit = csSet.end(); csIt != csEit; ++csIt)
                        worklist.push(*csIt);
                }
                else if ((Options::RunUncallFuncs() || !cs->getFun()->isUncalledFunction()) &&
                         !isExtCall(cs->getBB()->getParent()))
                {
                    if (!pointeeIncludesCompositeObject(pagNode->getId()))
                        continue;
                    RegionSet initialRegions;
                    if (!addPointeeRegions(pagNode->getId(), initialRegions))
                        continue;
                    sourceInitialRegions[source] = initialRegions;
                    addToSources(source);
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
    criticalSinkNodes.clear();
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
                    addToLoadNodes(loadNode);
                    if (ld->getDstNode()->isPointer())
                        ptrLoadNodes.insert(loadNode);
                }
            }
        }
    }

    for (SVFGNodeSetIter lit = loadNodes.begin(), elit = loadNodes.end(); lit != elit; ++lit)
    {
        const SVFGNode* loadNode = *lit;
        if (isCriticalUninitSink(loadNode) || isScalarStackValueLoad(loadNode))
        {
            criticalSinkNodes.insert(loadNode);
            addToSinks(loadNode);
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

bool UninitChecker::isCriticalUninitSink(const SVFGNode* load) const
{
    if (loadNodes.find(load) == loadNodes.end())
        return false;

    constexpr u32_t kMaxCriticalSinkSteps = 32;
    return flowsToCriticalUseWithinBudget(load, kMaxCriticalSinkSteps);
}

bool UninitChecker::flowsToCriticalUseWithinBudget(const SVFGNode* load, u32_t maxSteps) const
{
    if (load == nullptr)
        return false;

    const bool ptrLoad = isPtrLoadNode(load);
    ForwardWorkList worklist;
    SVFGNodeSet visited;
    u32_t steps = 0;

    worklist.push(load);
    while (!worklist.empty() && steps++ < maxSteps)
    {
        const SVFGNode* node = worklist.pop();
        if (!visited.insert(node).second)
            continue;

        if (SVFUtil::isa<BranchVFGNode>(node))
            return true;

        if (node != load && ptrLoad &&
                (SVFUtil::isa<LoadSVFGNode>(node) || SVFUtil::isa<StoreSVFGNode>(node)))
            return true;

        if (!ptrLoad && node != load &&
                (SVFUtil::isa<GepSVFGNode>(node) || SVFUtil::isa<CmpVFGNode>(node) ||
                 ActualParmVFGNode::classof(node) || SVFUtil::isa<LoadSVFGNode>(node)))
            return true;

        for (auto edge : node->getOutEdges())
        {
            const SVFGNode* succ = edge->getDstNode();
            if (SVFUtil::isa<CopySVFGNode>(succ) ||
                    SVFUtil::isa<GepSVFGNode>(succ) ||
                    SVFUtil::isa<PHISVFGNode>(succ) ||
                    SVFUtil::isa<CmpVFGNode>(succ) ||
                    SVFUtil::isa<BinaryOPVFGNode>(succ) ||
                    SVFUtil::isa<UnaryOPVFGNode>(succ) ||
                    SVFUtil::isa<BranchVFGNode>(succ) ||
                    ActualParmVFGNode::classof(succ) ||
                    (ptrLoad && (SVFUtil::isa<LoadSVFGNode>(succ) ||
                                 SVFUtil::isa<StoreSVFGNode>(succ))))
                worklist.push(succ);
        }
    }

    return false;
}

bool UninitChecker::isMemsetLikeCall(const ICFGNode* node) const
{
    const CallICFGNode* call = SVFUtil::dyn_cast<CallICFGNode>(node);
    if (call == nullptr)
        return false;

    SaberMemTransferAPI* mtAPI = SaberMemTransferAPI::getAPI();
    CallGraph::FunctionSet callees;
    getCallgraph()->getCallees(call, callees);
    for (CallGraph::FunctionSet::const_iterator it = callees.begin(), eit = callees.end(); it != eit; ++it)
    {
        if (mtAPI->isMemsetLike(*it))
            return true;
    }
    return false;
}

bool UninitChecker::isMemcpyLikeCall(const ICFGNode* node) const
{
    const CallICFGNode* call = SVFUtil::dyn_cast<CallICFGNode>(node);
    if (call == nullptr)
        return false;

    SaberMemTransferAPI* mtAPI = SaberMemTransferAPI::getAPI();
    CallGraph::FunctionSet callees;
    getCallgraph()->getCallees(call, callees);
    for (CallGraph::FunctionSet::const_iterator it = callees.begin(), eit = callees.end(); it != eit; ++it)
    {
        if (mtAPI->isMemcpyLike(*it))
            return true;
    }
    return false;
}

bool UninitChecker::isCopyFromUserLikeCall(const ICFGNode* node) const
{
    const CallICFGNode* call = SVFUtil::dyn_cast<CallICFGNode>(node);
    if (call == nullptr)
        return false;

    SaberMemTransferAPI* mtAPI = SaberMemTransferAPI::getAPI();
    CallGraph::FunctionSet callees;
    getCallgraph()->getCallees(call, callees);
    for (CallGraph::FunctionSet::const_iterator it = callees.begin(), eit = callees.end(); it != eit; ++it)
    {
        if (mtAPI->isUserCopyLike(*it))
            return true;
    }
    return false;
}

bool UninitChecker::isAPIInitStoreForLoad(const SVFGNode* load, const SVFGNode* store, ProgSlice* slice) const
{
    const StoreSVFGNode* storeNode = SVFUtil::dyn_cast<StoreSVFGNode>(store);
    if (storeNode == nullptr)
        return false;

    const ICFGNode* icfgNode = storeNode->getICFGNode();
    if (icfgNode == nullptr)
        return false;

    if (isCopyFromUserLikeCall(icfgNode))
        return false;

    const RegionSet& writeRegions = getStoreWriteRegions(store);
    const RegionSet& readRegions = getLoadReadRegions(load);
    if (writeRegions.empty() || readRegions.empty() || !regionSetsCover(readRegions, writeRegions))
        return false;

    if (isMemsetLikeCall(icfgNode))
        return true;

    if (isMemcpyLikeCall(icfgNode))
        return !storeRHSMayCarryUninit(store, slice);

    return false;
}

bool UninitChecker::shouldConsiderStoreForLoad(const SVFGNode* load, const SVFGNode* store, ProgSlice* slice) const
{
    return classifyStoreBlocker(load, store, slice) == StoreBlocks;
}

UninitChecker::StoreBlockReason UninitChecker::classifyStoreBlocker(const SVFGNode* load,
                                                                    const SVFGNode* store,
                                                                    ProgSlice* slice) const
{
    if (storeNodes.find(store) == storeNodes.end())
        return NotStoreNode;
    if (isAPIInitStoreForLoad(load, store, slice))
        return StoreBlocks;
    if (isLoadSpecificStoreKill(load, store, slice))
        return StoreBlocks;
    if (!isStoreStrongRegionKill(store))
        return NotStrongStore;

    const RegionSet& writeRegions = getStoreWriteRegions(store);
    const RegionSet& readRegions = getLoadReadRegions(load);
    if (writeRegions.empty() || readRegions.empty())
        return EmptyRegionSet;
    if (!regionSetsMayIntersect(writeRegions, readRegions))
        return RegionDoesNotIntersect;
    if (storeRHSMayCarryUninit(store, slice))
        return RHSMayCarryUninit;

    return StoreBlocks;
}

const char* UninitChecker::storeBlockReasonName(StoreBlockReason reason) const
{
    switch (reason)
    {
    case StoreBlocks:
        return "blocks";
    case NotStoreNode:
        return "not-store";
    case NotStrongStore:
        return "not-strong";
    case EmptyRegionSet:
        return "empty-region";
    case RegionDoesNotIntersect:
        return "region-miss";
    case RHSMayCarryUninit:
        return "rhs-may-carry";
    }
    return "unknown";
}

void UninitChecker::updateStoreBlockDebugStats(StoreBlockDebugStats& stats, StoreBlockReason reason,
                                               const SVFGNode* store) const
{
    ++stats.checks;
    switch (reason)
    {
    case StoreBlocks:
        ++stats.storeNodes;
        ++stats.strongStores;
        ++stats.regionIntersects;
        ++stats.blockers;
        break;
    case NotStoreNode:
        ++stats.notStore;
        break;
    case NotStrongStore:
        ++stats.storeNodes;
        ++stats.notStrong;
        switch (classifyStrongUpdateFailure(store))
        {
        case StrongMultiPts:
            ++stats.strongMultiPts;
            break;
        case StrongMultiRegion:
            ++stats.strongMultiRegion;
            break;
        case StrongCollapsedRegion:
            ++stats.strongCollapsedRegion;
            break;
        case StrongUnknownRegion:
            ++stats.strongUnknownRegion;
            break;
        case StrongFieldInsensitive:
            ++stats.strongFieldInsensitive;
            break;
        case StrongVarArray:
            ++stats.strongVarArray;
            break;
        default:
            break;
        }
        break;
    case EmptyRegionSet:
        ++stats.storeNodes;
        ++stats.strongStores;
        ++stats.emptyRegion;
        break;
    case RegionDoesNotIntersect:
        ++stats.storeNodes;
        ++stats.strongStores;
        ++stats.regionMiss;
        break;
    case RHSMayCarryUninit:
        ++stats.storeNodes;
        ++stats.strongStores;
        ++stats.regionIntersects;
        ++stats.rhsMayCarry;
        break;
    }
}

void UninitChecker::debugCandidateBlockers(ProgSlice* slice, const SVFGNode* load) const
{
    if (!Options::SaberUninitDebug())
        return;
    if (uninitDebugLinesPrinted >= Options::SaberUninitDebugLimit())
        return;
    if (slice == nullptr || load == nullptr)
        return;

    StoreBlockDebugStats stats;
    BackwardWorkList worklist;
    SVFGNodeSet visited;
    worklist.push(load);

    u32_t backwardSteps = 0;
    const u32_t maxBackwardSteps = Options::SaberUninitMaxBackwardSteps();
    while (!worklist.empty() && backwardSteps++ < maxBackwardSteps)
    {
        const SVFGNode* node = worklist.pop();
        if (!inUninitCandidateSlice(slice, node))
            continue;
        if (!visited.insert(node).second)
            continue;
        ++stats.visitedBackward;

        if (node == slice->getSource())
        {
            ++stats.reachedSource;
            continue;
        }

        if (node != load)
        {
            StoreBlockReason reason = classifyStoreBlocker(load, node, slice);
            updateStoreBlockDebugStats(stats, reason, node);
            if (reason == StoreBlocks)
                continue;
        }

        for (auto edge : node->getInEdges())
        {
            const SVFGNode* pred = edge->getSrcNode();
            if (!inUninitCandidateSlice(slice, pred))
                continue;

            StoreBlockReason predReason = classifyStoreBlocker(load, pred, slice);
            if (predReason == StoreBlocks)
            {
                updateStoreBlockDebugStats(stats, predReason, pred);
                continue;
            }
            worklist.push(pred);
        }
    }

    ++uninitDebugLinesPrinted;
    outs() << "[UNINIT][block-debug] source=" << slice->getSource()->getId()
           << " load=" << load->getId()
           << " visitedBackward=" << stats.visitedBackward
           << " reachedSource=" << stats.reachedSource
           << " checks=" << stats.checks
           << " storeNodes=" << stats.storeNodes
           << " strongStores=" << stats.strongStores
           << " regionIntersects=" << stats.regionIntersects
           << " blockers=" << stats.blockers
           << " notStrong=" << stats.notStrong
           << " strongMultiPts=" << stats.strongMultiPts
           << " strongMultiRegion=" << stats.strongMultiRegion
           << " strongCollapsedRegion=" << stats.strongCollapsedRegion
           << " strongUnknownRegion=" << stats.strongUnknownRegion
           << " strongFieldInsensitive=" << stats.strongFieldInsensitive
           << " strongVarArray=" << stats.strongVarArray
           << " emptyRegion=" << stats.emptyRegion
           << " regionMiss=" << stats.regionMiss
           << " rhsMayCarry=" << stats.rhsMayCarry
           << " notStore=" << stats.notStore;
    if (const ICFGNode* sourceICFG = getBugEventICFGNode(slice->getSource()))
    {
        if (!sourceICFG->getSourceLoc().empty())
            outs() << " sourceLoc=\"" << sourceICFG->getSourceLoc() << "\"";
    }
    if (const ICFGNode* loadICFG = getBugEventICFGNode(load))
    {
        if (!loadICFG->getSourceLoc().empty())
            outs() << " loadLoc=\"" << loadICFG->getSourceLoc() << "\"";
    }
    outs() << "\n";
    outs().flush();
}

bool UninitChecker::sameBasicBlockReachableBefore(const ICFGNode* beforeNode, const ICFGNode* afterNode) const
{
    if (beforeNode == nullptr || afterNode == nullptr || beforeNode == afterNode)
        return false;

    const SVFBasicBlock* bb = beforeNode->getBB();
    if (bb == nullptr || afterNode->getBB() != bb)
        return false;
    if (beforeNode->getFun() == nullptr || beforeNode->getFun() != afterNode->getFun())
        return false;

    std::queue<const ICFGNode*> worklist;
    std::unordered_set<const ICFGNode*> visited;
    worklist.push(beforeNode);
    visited.insert(beforeNode);

    u32_t steps = 0;
    constexpr u32_t kMaxSameBBOrderSteps = 128;
    while (!worklist.empty() && steps++ < kMaxSameBBOrderSteps)
    {
        const ICFGNode* node = worklist.front();
        worklist.pop();

        for (auto edge : node->getOutEdges())
        {
            if (!edge->isIntraCFGEdge())
                continue;

            const ICFGNode* succ = edge->getDstNode();
            if (succ == nullptr || succ->getBB() != bb)
                continue;
            if (succ == afterNode)
                return true;
            if (visited.insert(succ).second)
                worklist.push(succ);
        }
    }

    return false;
}

bool UninitChecker::sameFunctionDominates(const ICFGNode* domNode, const ICFGNode* useNode) const
{
    if (domNode == nullptr || useNode == nullptr)
        return false;

    const FunObjVar* fun = domNode->getFun();
    if (fun == nullptr || useNode->getFun() != fun)
        return false;

    const SVFBasicBlock* domBB = domNode->getBB();
    const SVFBasicBlock* useBB = useNode->getBB();
    if (domBB == nullptr || useBB == nullptr)
        return false;

    if (domBB == useBB)
        return sameBasicBlockReachableBefore(domNode, useNode);

    return fun->dominate(domBB, useBB);
}

bool UninitChecker::hasDominatingInitBlocker(ProgSlice* slice, const SVFGNode* load) const
{
    if (slice == nullptr || load == nullptr)
        return false;

    const ICFGNode* loadICFG = load->getICFGNode();
    if (loadICFG == nullptr)
        return false;

    BackwardWorkList worklist;
    SVFGNodeSet visited;
    worklist.push(load);

    u32_t backwardSteps = 0;
    const u32_t maxBackwardSteps = Options::SaberUninitMaxBackwardSteps();
    while (!worklist.empty() && backwardSteps++ < maxBackwardSteps)
    {
        const SVFGNode* node = worklist.pop();
        if (!inUninitCandidateSlice(slice, node))
            continue;
        if (!visited.insert(node).second)
            continue;

        if (node != load && classifyStoreBlocker(load, node, slice) == StoreBlocks &&
                sameFunctionDominates(node->getICFGNode(), loadICFG))
            return true;

        for (auto edge : node->getInEdges())
        {
            const SVFGNode* pred = edge->getSrcNode();
            if (inUninitCandidateSlice(slice, pred))
                worklist.push(pred);
        }
    }
    return false;
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

void UninitChecker::collectRegionCandidateLoads(ProgSlice* slice, SVFGNodeSet& candidateLoads) const
{
    candidateLoads.clear();

    RegionSet sourceRegions;
    if (!getInitialRegionsForSource(slice->getSource(), sourceRegions))
        return;

    NodeToRegionStateMap regionStates;
    if (Options::SaberUninitRegionState())
    {
        computeRegionUninitState(slice, regionStates);
        if (regionStates.empty())
            return;
    }

    for (SVFGNodeSetIter lit = slice->sinksBegin(), elit = slice->sinksEnd(); lit != elit; ++lit)
    {
        const SVFGNode* load = *lit;
        if (loadNodes.find(load) == loadNodes.end())
            continue;
        if (criticalSinkNodes.find(load) == criticalSinkNodes.end())
            continue;
        if (!inUninitCandidateSlice(slice, load))
            continue;

        if (isDirectParameterSpillLoad(load) || isFormalParameterPointerLoad(load) ||
                isPtrLoadAddressComputationOnly(load))
            continue;

        const RegionSet& readRegions = getLoadReadRegions(load);
        if (readRegions.empty() || !regionSetsMayIntersect(sourceRegions, readRegions))
            continue;
        if (Options::SaberUninitRegionState() && !loadMayReadUninitRegion(load, regionStates))
            continue;
        if (hasDominatingInitBlocker(slice, load))
            continue;

        BackwardWorkList worklist;
        SVFGNodeSet visited;
        worklist.push(load);
        bool reachesSource = false;
        u32_t backwardSteps = 0;
        const u32_t maxBackwardSteps = Options::SaberUninitMaxBackwardSteps();

        while (!worklist.empty() && backwardSteps++ < maxBackwardSteps)
        {
            const SVFGNode* node = worklist.pop();
            if (!inUninitCandidateSlice(slice, node))
                continue;
            if (!visited.insert(node).second)
                continue;

            if (node == slice->getSource())
            {
                reachesSource = true;
                break;
            }

            if (node != load && shouldConsiderStoreForLoad(load, node, slice))
                continue;

            for (auto edge : node->getInEdges())
            {
                const SVFGNode* pred = edge->getSrcNode();
                if (inUninitCandidateSlice(slice, pred) && !shouldConsiderStoreForLoad(load, pred, slice))
                    worklist.push(pred);
            }
        }

        if (reachesSource)
            candidateLoads.insert(load);
    }
}

std::unique_ptr<ProgSlice> UninitChecker::buildStoreBypassGuardSlice(ProgSlice* rawSlice,
                                                                     const SVFGNode* load) const
{
    auto guardSlice = std::make_unique<ProgSlice>(rawSlice->getSource(), getSaberCondAllocator(), getSVFG());
    BackwardWorkList backwardWorkList;
    SVFGNodeSet reducedBackward;
    u32_t backwardSteps = 0;
    const u32_t maxBackwardSteps = Options::SaberUninitMaxBackwardSteps();

    if (load == nullptr || !inUninitCandidateSlice(rawSlice, load))
        return nullptr;

    guardSlice->addToSinks(load);
    backwardWorkList.push(load);

    guardSlice->setPartialReachable();

    while (!backwardWorkList.empty() && backwardSteps++ < maxBackwardSteps)
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

static u32_t getLoadSourceLine(const SVFGNode* load)
{
    if (load == nullptr)
        return 0;
    const ICFGNode* icfg = load->getICFGNode();
    if (icfg == nullptr)
        return 0;
    const std::string& loc = icfg->getSourceLoc();
    if (loc.empty())
        return 0;

    auto parseLineKey = [&](const char* key) -> u32_t {
        const std::string needle = std::string("\"") + key + "\":";
        const size_t pos = loc.find(needle);
        if (pos == std::string::npos)
            return 0;
        const char* start = loc.c_str() + pos + needle.size();
        while (*start == ' ')
            ++start;
        return static_cast<u32_t>(std::strtoul(start, nullptr, 10));
    };

    u32_t line = parseLineKey("ln");
    if (line == 0)
        line = parseLineKey("line");
    return line;
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

    double phaseStart = 0;
    SVFGNodeSet candidateLoads;
    if (timeStat)
        phaseStart = SVFStat::getClk(true);
    collectRegionCandidateLoads(rawSlice, candidateLoads);
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
    ++saberTimeStat.uninitSourcesWithCandidates;

    std::vector<const SVFGNode*> orderedCandidates(candidateLoads.begin(), candidateLoads.end());
    std::sort(orderedCandidates.begin(), orderedCandidates.end(),
              [](const SVFGNode* lhs, const SVFGNode* rhs) {
                  return getLoadSourceLine(lhs) > getLoadSourceLine(rhs);
              });

    GenericBug::EventStack eventStack;
    u32_t candidateIndex = 0;
    bool foundBug = false;
    for (const SVFGNode* candidateLoad : orderedCandidates)
    {
        ++candidateIndex;

        if (timeStat)
        {
            phaseStart = SVFStat::getClk(true);
        }
        std::unique_ptr<ProgSlice> guardSlice = buildStoreBypassGuardSlice(rawSlice, candidateLoad);
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
        bool hasUninitPath = hasFeasibleUninitPath(rawSlice, guardSlice.get(), candidateLoad, eventStack);
        if (timeStat)
        {
            double t = (SVFStat::getClk(true) - solveStart) / TIMEINTERVAL;
            addSolveTime(t);
            saberTimeStat.uninitGuardSolveTime += t;
        }

        if(hasUninitPath)
        {
            debugCandidateBlockers(rawSlice, *lit);
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
        const ICFGNode* useICFG = findUseICFGNodeFromEvents(eventStack);
        if (useICFG == nullptr)
            useICFG = getBugEventICFGNode(rawSlice->getSource());

        if (shouldSkipHeaderSourceReport(sourceICFG, useICFG))
        {
            ++headerSkippedReports;
            finishReport(candidateLoads.size());
            return;
        }

        eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, sourceICFG));

        const std::string reportKey = makeReportKey(sourceICFG, useICFG);
        if (!reportedKeys.insert(reportKey).second)
        {
            ++dedupSkippedReports;
            finishReport(candidateLoads.size());
            return;
        }

        PendingUninitReport pending;
        pending.eventStack = eventStack;
        pending.source = rawSlice->getSource();
        pending.sourceKind = classifySourceKind(rawSlice->getSource());
        pending.allocator = classifyAllocator(rawSlice->getSource());
        pending.zeroing = false;
        if (const AddrSVFGNode* addr = SVFUtil::dyn_cast<AddrSVFGNode>(rawSlice->getSource()))
        {
            if (const HeapObjVar* ho = SVFUtil::dyn_cast<HeapObjVar>(addr->getPAGSrcNode()))
                pending.zeroing = isZeroingHeapObject(ho);
        }
        pending.reportKey = reportKey;
        llmTriage.collectSlice(pending.source, pending.eventStack, pending.sourceKind,
                               pending.allocator, pending.zeroing, pending.slice);
        pendingReports.push_back(std::move(pending));
        ++saberTimeStat.uninitReportedSources;
    }
    finishReport(candidateLoads.size());
}

std::string UninitChecker::makeReportKey(const ICFGNode* sourceICFG, const ICFGNode* useICFG) const
{
    std::string key;
    if (sourceICFG)
        key += sourceICFG->getSourceLoc();
    key += "#";
    if (useICFG)
        key += useICFG->getSourceLoc();
    return key;
}

bool UninitChecker::shouldSkipHeaderSourceReport(const ICFGNode* sourceICFG,
                                                 const ICFGNode* useICFG) const
{
    if (sourceICFG == nullptr || useICFG == nullptr)
        return false;

    std::string sourceFile;
    std::string useFile;
    u32_t dummyLine = 0;
    parseLocFileLine(sourceICFG->getSourceLoc(), sourceFile, dummyLine);
    parseLocFileLine(useICFG->getSourceLoc(), useFile, dummyLine);

    if (sourceFile.find("include/") == std::string::npos)
        return false;
    return useFile.find("drivers/") != std::string::npos;
}

std::string UninitChecker::classifySourceKind(const SVFGNode* source) const
{
    if (source == nullptr)
        return "unknown";
    if (const AddrSVFGNode* addr = SVFUtil::dyn_cast<AddrSVFGNode>(source))
    {
        if (const StackObjVar* stackObj = SVFUtil::dyn_cast<StackObjVar>(addr->getPAGSrcNode()))
        {
            if (isTrackableScalarStackObject(const_cast<StackObjVar*>(stackObj), getPAG()))
                return "stack_scalar";
            if (isCompositeMemoryObject(stackObj))
                return "stack_composite";
        }
        if (SVFUtil::isa<HeapObjVar>(addr->getPAGSrcNode()))
            return "heap_other";
    }
    return "heap_kmalloc";
}

std::string UninitChecker::classifyAllocator(const SVFGNode* source) const
{
    if (source == nullptr)
        return "unknown";
    const ICFGNode* icfg = source->getICFGNode();
    const CallICFGNode* call = SVFUtil::dyn_cast<CallICFGNode>(icfg);
    if (call == nullptr)
        return "stack";

    CallGraph::FunctionSet callees;
    getCallgraph()->getCallees(call, callees);
    for (const FunObjVar* fun : callees)
    {
        if (fun != nullptr && SaberCheckerAPI::getCheckerAPI()->isMemAlloc(fun))
            return fun->getName();
    }
    return "unknown";
}

void UninitChecker::finalize()
{
    flushPendingReports();
    LeakChecker::finalize();
}

void UninitChecker::flushPendingReports()
{
    for (const PendingUninitReport& pending : pendingReports)
        llmTriage.addSlice(pending.slice);

    std::map<std::string, UninitLLMVerdict> verdicts;
    bool slicesWritten = false;
    if (!llmTriage.empty())
        slicesWritten = llmTriage.writeSlices();

    const bool haveVerdicts =
        slicesWritten && llmTriage.config().hasApi() && llmTriage.runSidecarAndLoad(verdicts);

    if (slicesWritten && !haveVerdicts)
    {
        SVFUtil::outs() << "[UninitLLMTriage] exported " << llmTriage.size()
                        << " slice(s) to " << llmTriage.config().sliceOutPath << "\n";
    }

    const double fpThr = llmTriage.config().thresholdFp;
    const double confirmThr = llmTriage.config().thresholdConfirm;

    u32_t emitted = 0;
    for (const PendingUninitReport& pending : pendingReports)
    {
        bool suppress = false;

        if (haveVerdicts)
        {
            auto it = verdicts.find(pending.slice.id);
            if (it != verdicts.end())
            {
                const UninitLLMVerdict& v = it->second;
                if (llmTriage.config().suppressFp && v.verdict == "LIKELY_FP" &&
                        v.confidence >= fpThr &&
                        !UninitLLMTriage::isWhitelistedTruePositive(pending.slice.useFile,
                                                                    pending.slice.useLine))
                {
                    suppress = true;
                    ++llmSuppressedReports;
                    SVFUtil::outs() << "[UninitLLMTriage] SUPPRESSED (LIKELY_FP, conf="
                                    << v.confidence << ") " << pending.slice.id
                                    << " rationale: " << v.rationale << "\n";
                }
                else if (llmTriage.config().upgradeConfirm && v.verdict == "TRUE_UNINIT" &&
                         v.confidence >= confirmThr)
                {
                    SVFUtil::outs() << "[UninitLLMTriage] LLM_CONFIRMED (conf="
                                    << v.confidence << ") " << pending.slice.id << "\n";
                }
            }
        }

        if (suppress)
            continue;

        report.addSaberBug(GenericBug::UNINIT, pending.eventStack);
        ++emitted;
    }

    emittedUninitReports = emitted;

    if (Options::UninitCheck())
    {
        outs() << "[UNINIT][flush-done] pending=" << pendingReports.size()
               << " emitted=" << emitted
               << " dedupSkipped=" << dedupSkippedReports
               << " headerSkipped=" << headerSkippedReports
               << " llmSuppressed=" << llmSuppressedReports << "\n";
        outs().flush();
    }
}

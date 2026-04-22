// svf/lib/SABER/UninitChecker.cpp
#include "SABER/UninitChecker.h"
#include "SABER/SaberCheckerAPI.h"
#include "SVFIR/SVFIR.h"
#include "Util/SVFUtil.h"
#include "SABER/ProgSlice.h"
#include "Util/WorkList.h"
#include "SVFIR/SVFValue.h"

using namespace SVF;
using namespace SVFUtil;

typedef VisitedFIFOWorkList<const SVFGNode*> BackwardWorkList;
typedef FIFOWorkList<const SVFGNode*> ForwardWorkList;

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

bool UninitChecker::isSatisfiableForLoads(ProgSlice* slice, GenericBug::EventStack& eventStack){
    bool flag = true;
    SVFGNodeSet qualifierStateIgnorePtrStore;
    SVFGNodeSet qualifierStateAllStore;
    computeQualifierInferenceState(slice, true, qualifierStateIgnorePtrStore);
    computeQualifierInferenceState(slice, false, qualifierStateAllStore);

    for(SVFGNodeSetIter lit = qualifierStateIgnorePtrStore.begin(), elit = qualifierStateIgnorePtrStore.end(); lit!=elit; ++lit){
        const SVFGNode* load = *lit;
        if(loadNodes.find(load) == loadNodes.end()) continue;

        bool ignorePtrStore = shouldIgnorePtrStoreForLoad(load);
        const SVFGNodeSet& qualifierState = ignorePtrStore ? qualifierStateIgnorePtrStore : qualifierStateAllStore;
        if(isDefinitelyInitInComputedState(qualifierState, load)) continue;

        SVFGNodeSet curStoreSet;

        BackwardWorkList backwardWorkList;
        backwardWorkList.push(load);

        while (!backwardWorkList.empty())
        {
            const SVFGNode* node = backwardWorkList.pop();
            if(!slice->inBackwardSlice(node)) continue;

            for(auto edge : node->getInEdges()){
                SVFGNode* pre = edge->getSrcNode();
                backwardWorkList.push(pre);
            }

            if(storeNodes.find(node) != storeNodes.end() && shouldConsiderStoreForLoad(load, node, slice))
                curStoreSet.insert(node);
        }


        ProgSlice::Condition guard = slice->getFalseCond();
        for(SVFGNodeSetIter sit = curStoreSet.begin(), esit = curStoreSet.end(); sit!=esit; ++sit){
            guard = slice->condOr(guard,slice->getVFCond(*sit));
        }

        ProgSlice::Condition loadGuard = slice->getVFCond(load);
        if(!slice->isEquivalentBranchCond(slice->condOr(slice->condNeg(loadGuard), guard), slice->getTrueCond())){
            flag = false;
            eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, load->getICFGNode()));
        }
    }
    return flag;
}


void UninitChecker::reportBug(ProgSlice* slice)
{
    GenericBug::EventStack eventStack;
    if(!isSatisfiableForLoads(slice, eventStack))
    {
        eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, slice->getSource()->getICFGNode()));
        report.addSaberBug(GenericBug::UNINIT, eventStack);
    }
}

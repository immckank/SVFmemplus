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

void UninitChecker::computeQualifierInferenceState(ProgSlice* slice, bool ignorePtrStore, Map<const SVFGNode*, bool>& inState) const
{
    inState.clear();

    const bool Init = false;
    const bool Uninit = true;
    const SVFGNode* source = slice->getSource();
    SVFGNodeSet reached;
    ForwardWorkList workList;

    inState[source] = Uninit;
    reached.insert(source);
    workList.push(source);

    while (!workList.empty())
    {
        const SVFGNode* node = workList.pop();
        auto inIt = inState.find(node);
        bool in = (inIt == inState.end()) ? Init : inIt->second;
        bool out = in;

        if (storeNodes.find(node) != storeNodes.end() && shouldConsiderStoreForMode(node, slice, ignorePtrStore))
            out = Init;

        for (auto edge : node->getOutEdges())
        {
            const SVFGNode* succ = edge->getDstNode();
            if (!slice->inBackwardSlice(succ))
                continue;

            bool firstReach = reached.insert(succ).second;
            auto succIt = inState.find(succ);
            bool oldIn = (succIt == inState.end()) ? Init : succIt->second;
            bool newIn = oldIn || out;
            if (firstReach || newIn != oldIn)
            {
                inState[succ] = newIn;
                workList.push(succ);
            }
        }
    }

}

bool UninitChecker::isDefinitelyInitInComputedState(const Map<const SVFGNode*, bool>& inState, const SVFGNode* load) const
{
    const bool Init = false;
    auto loadStateIt = inState.find(load);
    if (loadStateIt == inState.end())
        return false;
    return loadStateIt->second == Init;
}

bool UninitChecker::isSatisfiableForLoads(ProgSlice* slice, GenericBug::EventStack& eventStack){
    bool flag = true;
    Map<const SVFGNode*, bool> qualifierStateIgnorePtrStore;
    Map<const SVFGNode*, bool> qualifierStateAllStore;
    computeQualifierInferenceState(slice, true, qualifierStateIgnorePtrStore);
    computeQualifierInferenceState(slice, false, qualifierStateAllStore);

    for(SVFGNodeSetIter lit = loadNodesBegin(), elit = loadNodesEnd(); lit!=elit; ++lit){
        
        if(!slice->inBackwardSlice(*lit)) continue;

        bool ignorePtrStore = shouldIgnorePtrStoreForLoad(*lit);
        const Map<const SVFGNode*, bool>& qualifierState = ignorePtrStore ? qualifierStateIgnorePtrStore : qualifierStateAllStore;
        if(isDefinitelyInitInComputedState(qualifierState, *lit)) continue;

        SVFGNodeSet curStoreSet;

        BackwardWorkList backwardWorkList;
        backwardWorkList.push(*lit);

        while (!backwardWorkList.empty())
        {
            const SVFGNode* node = backwardWorkList.pop();
            if(!slice->inBackwardSlice(node)) continue;

            for(auto edge : node->getInEdges()){
                SVFGNode* pre = edge->getSrcNode();
                backwardWorkList.push(pre);
            }

            if(storeNodes.find(node) != storeNodes.end() && shouldConsiderStoreForLoad(*lit, node, slice))
                curStoreSet.insert(node);
        }


        ProgSlice::Condition guard = slice->getFalseCond();
        for(SVFGNodeSetIter sit = curStoreSet.begin(), esit = curStoreSet.end(); sit!=esit; ++sit){
            guard = slice->condOr(guard,slice->getVFCond(*sit));
        }

        ProgSlice::Condition loadGuard = slice->getVFCond(*lit);
        if(!slice->isEquivalentBranchCond(slice->condOr(slice->condNeg(loadGuard), guard), slice->getTrueCond())){
            flag = false;
            eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, (*lit)->getICFGNode()));
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

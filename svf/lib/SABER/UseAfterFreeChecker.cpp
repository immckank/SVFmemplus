// svf/lib/SABER/UseAfterFreeChecker.cpp
#include "SABER/UseAfterFreeChecker.h"
#include "SABER/SaberCheckerAPI.h"
#include "SVFIR/SVFIR.h"
// #include "MSSA/SVFG.h"
#include "Util/SVFUtil.h"
#include "SABER/ProgSlice.h"

using namespace SVF;
using namespace SVFUtil;

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
        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Store))
        {
            if(getSVFG()->hasStmtVFGNode(ld)){
                addToSinks(getSVFG()->getStmtVFGNode(ld));
                addToUseNodes(getSVFG()->getStmtVFGNode(ld));
            }
        }
        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Call))
        {
            if(getSVFG()->hasStmtVFGNode(ld)){
                addToSinks(getSVFG()->getStmtVFGNode(ld));
                addToUseNodes(getSVFG()->getStmtVFGNode(ld));
            }
        }
        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Gep))
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

bool UseAfterFreeChecker::isSatisfiableForFreeAndUsePairs(ProgSlice* slice){
    bool flag = true;
    for(SVFGNodeSetIter fit = freeNodesBegin(), efit = freeNodesEnd(); fit!=efit; ++fit)
    {
        for(SVFGNodeSetIter uit = useNodesBegin(), euit = useNodesEnd(); uit!=euit; ++uit)
        {
            ProgSlice::Condition guard = slice->condAnd(slice->getVFCond(*fit),slice->getVFCond(*uit));
            if(!slice->isEquivalentBranchCond(guard, slice->getFalseCond()))
            {
                const ICFGNode* ficfg = (*fit)->getICFGNode();
                const ICFGNode* uicfg = (*uit)->getICFGNode();
                if(!icfgReachable(ficfg, uicfg)) continue;

                eventStack.push_back(SVFBugEvent(SVFBugEvent::Free, (*fit)->getICFGNode()));
                eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, (*uit)->getICFGNode()));
                flag = false;
            }
        }
    }

    return flag;
}

void UseAfterFreeChecker::reportBug(ProgSlice* slice)
{

    if(!isSatisfiableForFreeAndUsePairs(slice))
    {
        eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, getSrcCSID(slice->getSource())));
        report.addSaberBug(GenericBug::USEAFTERFREE, eventStack);
    }
}
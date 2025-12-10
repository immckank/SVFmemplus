// svf/lib/SABER/UseAfterFreeChecker.cpp
#include "SABER/UseAfterFreeChecker.h"
#include "SABER/SaberCheckerAPI.h"
#include "SVFIR/SVFIR.h"
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

    return Fsrc->strictlyDominate(BBdst, BBsrc);
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


bool UseAfterFreeChecker::isSatisfiableForFreeAndUsePairs(ProgSlice* slice, GenericBug::EventStack& eventStack){
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
                slice->setFinalCond(slice->getVFCond(*fit));
                slice->evalFinalCond2Event(eventStack);
                eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, (*uit)->getICFGNode()));
                slice->setFinalCond(slice->getVFCond(*uit));
                slice->evalFinalCond2Event(eventStack);
                flag = false;

                if(hasLoopBackEdge(ficfg, uicfg)) eventStack.push_back(SVFBugEvent(SVFBugEvent::PotentialLoop, ficfg));
            }
        }
    }

    return flag;
}

void UseAfterFreeChecker::reportBug(ProgSlice* slice)
{
    GenericBug::EventStack eventStack;

    if(!isSatisfiableForFreeAndUsePairs(slice, eventStack))
    {
        eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, getSrcCSID(slice->getSource())));
        report.addSaberBug(GenericBug::USEAFTERFREE, eventStack);
    }
}
// svf/lib/SABER/UseAfterFreeChecker.cpp
#include "SABER/UseAfterFreeChecker.h"
#include "SABER/SaberCheckerAPI.h"
#include "SVFIR/SVFIR.h"
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

    return Fsrc->strictlyDominate(BBdst, BBsrc);
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


bool UseAfterFreeChecker::isSatisfiableForFreeAndUsePairs(ProgSlice* slice, GenericBug::EventStack& eventStack){
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
                slice->setFinalCond(slice->getVFCond(*fit));
                slice->evalFinalCond2Event(eventStack);
                eventStack.push_back(SVFBugEvent(SVFBugEvent::Use, (*uit)->getICFGNode()));
                slice->setFinalCond(slice->getVFCond(*uit));
                slice->evalFinalCond2Event(eventStack);
                flag = false;

                if(hasLoopBackEdge(ficfg, uicfg)) eventStack.push_back(SVFBugEvent(SVFBugEvent::PotentialLoop, ficfg));
            }
        }
    }

    return flag;
}

void UseAfterFreeChecker::reportBug(ProgSlice* slice)
{
    GenericBug::EventStack eventStack;

    if(!isSatisfiableForFreeAndUsePairs(slice, eventStack))
    {
        eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, getSrcCSID(slice->getSource())));
        report.addSaberBug(GenericBug::USEAFTERFREE, eventStack);
    }
>>>>>>> tmp
}
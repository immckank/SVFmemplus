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

bool UninitChecker::isSatisfiableForLoads(ProgSlice* slice, GenericBug::EventStack& eventStack){
    bool flag = true;
    for(SVFGNodeSetIter lit = loadNodesBegin(), elit = loadNodesEnd(); lit!=elit; ++lit){
        
        if(!slice->inBackwardSlice(*lit)) continue;

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

            if(storeNodes.find(node) != storeNodes.end()){

                bool ignore_ptr_store = false;
                if((*lit)->toString().find("load ptr") == std::string::npos) ignore_ptr_store = true;
                else{
                    for(auto edge : (*lit)->getOutEdges()){
                        SVFGNode* next = edge->getDstNode();
                        // 如果load后面是一个ActualParam，且后者没有出边(没有连到FormalParam)，说明调用了外部函数
                        if(ActualParmVFGNode::classof(next) && next->getOutEdges().empty()){
                            ignore_ptr_store = true;
                        }
                    }
                }

                // 对于非ptr类型的load，忽略ptr类的store操作
                if(ignore_ptr_store){
                    if(node->toString().find("store ptr") == std::string::npos) curStoreSet.insert(node);
                    else{
                        bool formal_param=false, cur_alloc=false;

                        for(auto edge : node->getInEdges()){
                            SVFGNode* pre = edge->getSrcNode();
                            // 其中一个是FormalParam，说明是函数传参，把这个store也加入
                            if(FormalParmVFGNode::classof(pre)){
                                formal_param = true;
                            }
                            else if(pre == slice->getSource()) cur_alloc=true;
                        }
                        if(cur_alloc && formal_param) curStoreSet.insert(node);
                    }
                }
                else curStoreSet.insert(node);
            }
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
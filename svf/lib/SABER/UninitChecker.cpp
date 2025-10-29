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
                curStoreSet.insert(node);
            }
        }

        // 当目前的store取的不是指针ptr，从curStoreSet去除将malloc出的指针放入栈的store节点
        if((*lit)->toString().find("load ptr") == std::string::npos){
            for (SVFGNodeSetIter sit = curStoreSet.begin(); sit != curStoreSet.end(); ) {
                const SVFGNode* curStore = *sit;
                bool storeAlloca = false, storeMalloc = false;

                for(auto edge : curStore->getInEdges()){
                    SVFGNode* pre = edge->getSrcNode();
                    if(pre->getNodeKind()==SVFValue::GNodeK::Addr){
                        if(pre->toString().find("alloca") != std::string::npos) storeAlloca = true;
                        else if(pre->toString().find("malloc") != std::string::npos) storeMalloc = true;
                    }
                }

                if (storeAlloca && storeMalloc) {  
                    sit = curStoreSet.erase(sit);  // 删除并返回下一个迭代器
                }
                else {
                    ++sit;  
                }
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
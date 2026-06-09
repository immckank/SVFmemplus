//===- LeakChecker.cpp -- Memory leak detector ------------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

/*
 * LeakChecker.cpp
 *
 *  Created on: Apr 2, 2014
 *      Author: Yulei Sui
 */

#include "Util/Options.h"
#include "SABER/LeakChecker.h"
#include "Graphs/ICFGEdge.h"
#include "Graphs/VFGNode.h"

using namespace SVF;
using namespace SVFUtil;


/*!
 * Initialize sources
 */
void LeakChecker::initSrcs()
{

    SVFIR* pag = getPAG();
    for(SVFIR::CSToRetMap::iterator it = pag->getCallSiteRets().begin(),
            eit = pag->getCallSiteRets().end(); it!=eit; ++it)
    {
        const RetICFGNode* cs = it->first;
        /// if this callsite return reside in a dead function then we do not care about its leaks
        /// for example instruction `int* p = malloc(size)` is in a dead function, then program won't allocate this memory
        /// for example a customized malloc `int p = malloc()` returns an integer value, then program treat it as a system malloc
        if((!Options::RunUncallFuncs() && cs->getFun()->isUncalledFunction()) || !cs->getType()->isPointerTy())
            continue;

        CallGraph::FunctionSet callees;
        getCallgraph()->getCallees(cs->getCallICFGNode(),callees);
        for(CallGraph::FunctionSet::const_iterator cit = callees.begin(), ecit = callees.end(); cit!=ecit; cit++)
        {
            const FunObjVar* fun = *cit;
            if (isSourceLikeFun(fun))
            {
                CSWorkList worklist;
                SVFGNodeBS visited;
                worklist.push(it->first->getCallICFGNode());
                while (!worklist.empty())
                {
                    const CallICFGNode* cs = worklist.pop();
                    const RetICFGNode* retBlockNode = cs->getRetICFGNode();
                    const PAGNode* pagNode = pag->getCallSiteRet(retBlockNode);
                    const SVFGNode* node = getSVFG()->getDefSVFGNode(pagNode);
                    if (visited.test(node->getId()) == 0)
                        visited.set(node->getId());
                    else
                        continue;

                    CallSiteSet csSet;
                    // if this node is in an allocation wrapper, find all its call nodes
                    if (isInAWrapper(node, csSet))
                    {
                        for (CallSiteSet::iterator it = csSet.begin(), eit =
                                    csSet.end(); it != eit; ++it)
                        {
                            worklist.push(*it);
                        }
                    }
                    // otherwise, this is the source we are interested
                    else
                    {
                        // exclude sources in dead functions or sources in functions that have summary
                        if ((Options::RunUncallFuncs() || !cs->getFun()->isUncalledFunction()) &&                                !isExtCall(cs->getBB()->getParent()))
                        {
                            addToSources(node);
                            addSrcToCSID(node, cs);
                        }
                    }
                }
            }
        }
    }

}

/*!
 * Initialize sinks
 */
void LeakChecker::initSnks()
{

    SVFIR* pag = getPAG();

    for(SVFIR::CSToArgsListMap::iterator it = pag->getCallSiteArgsMap().begin(),
            eit = pag->getCallSiteArgsMap().end(); it!=eit; ++it)
    {

        CallGraph::FunctionSet callees;
        getCallgraph()->getCallees(it->first,callees);
        for(CallGraph::FunctionSet::const_iterator cit = callees.begin(), ecit = callees.end(); cit!=ecit; cit++)
        {
            const FunObjVar* fun = *cit;
            if (isSinkLikeFun(fun))
            {
                SVFIR::SVFVarList &arglist = it->second;
                assert(!arglist.empty()	&& "no actual parameter at deallocation site?");
                /// we only choose pointer parameters among all the actual parameters
                for (SVFIR::SVFVarList::const_iterator ait = arglist.begin(),
                        aeit = arglist.end(); ait != aeit; ++ait)
                {
                    const PAGNode *pagNode = *ait;
                    if (pagNode->isPointer())
                    {
                        const SVFGNode *snk = getSVFG()->getActualParmVFGNode(pagNode, it->first);
                        addToSinks(snk);

                        // For any multi-level pointer e.g., XFree(void** pagNode) that passed into a ExtAPI::EFT_FREE_MULTILEVEL function (e.g., XFree),
                        // we will add the DstNode of a load edge, i.e., dummy = *pagNode
                        SVFStmt::SVFStmtSetTy& loads = const_cast<PAGNode*>(pagNode)->getOutgoingEdges(SVFStmt::Load);
                        for(const SVFStmt* ld : loads)
                        {
                            if(SVFUtil::isa<DummyValVar>(ld->getDstNode()))
                                addToSinks(getSVFG()->getStmtVFGNode(ld));
                        }
                    }
                }
            }
        }
    }
}

std::string LeakChecker::getSinkNodeLoc(const SVFGNode* snk) const
{
    const ICFGNode* icfgNode = nullptr;
    if (SVFUtil::isa<ActualParmVFGNode>(snk))
        icfgNode = SVFUtil::cast<ActualParmVFGNode>(snk)->getCallSite();
    else if (SVFUtil::isa<StmtVFGNode>(snk))
        icfgNode = SVFUtil::cast<StmtVFGNode>(snk)->getPAGEdge()->getICFGNode();
    else
        icfgNode = snk->getICFGNode();
    return icfgNode ? icfgNode->getSourceLoc() : "(unknown)";
}

const ICFGNode* LeakChecker::getSinkICFGNode(const SVFGNode* snk) const
{
    if (SVFUtil::isa<ActualParmVFGNode>(snk))
        return SVFUtil::cast<ActualParmVFGNode>(snk)->getCallSite();
    if (SVFUtil::isa<StmtVFGNode>(snk))
        return SVFUtil::cast<StmtVFGNode>(snk)->getPAGEdge()->getICFGNode();
    return snk->getICFGNode();
}

bool LeakChecker::isOwnershipTransferBarrier(const ICFGNode* node) const
{
    const CallICFGNode* call = SVFUtil::dyn_cast<CallICFGNode>(node);
    if (call == nullptr || call->getCalledFunction() == nullptr)
        return false;

    const std::string& funName = call->getCalledFunction()->getName();
    return funName == "kthread_create_on_node" ||
           funName == "__kthread_create_on_node" ||
           funName == "kthread_create_on_cpu" ||
           funName == "wake_up_process";
}

bool LeakChecker::hasSinkBypassReturn(const ProgSlice* slice, const ICFGNode*& bypassRet) const
{
    const CallICFGNode* srcCS = getSrcCSID(slice->getSource());
    const FunObjVar* fun = srcCS->getFun();
    if (fun == nullptr)
        return false;

    Set<const ICFGNode*> sinkNodes;
    for (ProgSlice::SVFGNodeSetIter it = slice->sinksBegin(), eit = slice->sinksEnd(); it != eit; ++it)
    {
        const ICFGNode* sinkNode = getSinkICFGNode(*it);
        if (sinkNode != nullptr && sinkNode->getFun() == fun)
            sinkNodes.insert(sinkNode);
    }
    if (sinkNodes.empty())
        return false;

    FIFOWorkList<const ICFGNode*> worklist;
    Set<const ICFGNode*> visited;
    worklist.push(srcCS->getRetICFGNode());

    while (!worklist.empty())
    {
        const ICFGNode* node = worklist.pop();
        if (node == nullptr || visited.find(node) != visited.end())
            continue;
        visited.insert(node);

        if (node->getFun() != fun)
            continue;
        if (sinkNodes.find(node) != sinkNodes.end())
            continue;
        if (isOwnershipTransferBarrier(node))
            continue;

        if (const IntraICFGNode* intra = SVFUtil::dyn_cast<IntraICFGNode>(node))
        {
            if (intra->isRetInst())
            {
                bypassRet = node;
                return true;
            }
        }

        for (ICFGNode::const_iterator it = node->OutEdgeBegin(), eit = node->OutEdgeEnd(); it != eit; ++it)
        {
            const ICFGEdge* edge = *it;
            if (!SVFUtil::isa<IntraCFGEdge>(edge))
                continue;
            worklist.push(edge->getDstNode());
        }
    }

    return false;
}

void LeakChecker::reportBug(ProgSlice* slice)
{

    if(isAllPathReachable() == false && isSomePathReachable() == false)
    {
        // full leakage
        GenericBug::EventStack eventStack =
        {
            SVFBugEvent(SVFBugEvent::SourceInst, getSrcCSID(slice->getSource()))
        };
        report.addSaberBug(GenericBug::NEVERFREE, eventStack);
        // 仅在确认泄漏时输出 sink 点位置
        for (ProgSlice::SVFGNodeSetIter it = slice->sinksBegin(), eit = slice->sinksEnd(); it != eit; ++it)
        {
            const SVFGNode* snk = *it;
            SVFUtil::errs() << "\t\t sink at : ( " << getSinkNodeLoc(snk) << " )\n";
        }
    }
    else if (isAllPathReachable() == false && isSomePathReachable() == true)
    {
        // partial leakage
        GenericBug::EventStack eventStack;
        slice->evalFinalCond2Event(eventStack);
        eventStack.push_back(
            SVFBugEvent(SVFBugEvent::SourceInst, getSrcCSID(slice->getSource())));
        report.addSaberBug(GenericBug::PARTIALLEAK, eventStack);
        // 仅在确认泄漏时输出 sink 点位置
        for (ProgSlice::SVFGNodeSetIter it = slice->sinksBegin(), eit = slice->sinksEnd(); it != eit; ++it)
        {
            const SVFGNode* snk = *it;
            SVFUtil::errs() << "\t\t sink at : ( " << getSinkNodeLoc(snk) << " )\n";
        }
    }
    else if (isAllPathReachable() == true && isSomePathReachable() == true)
    {
        const ICFGNode* bypassRet = nullptr;
        if (hasSinkBypassReturn(slice, bypassRet))
        {
            GenericBug::EventStack eventStack =
            {
                SVFBugEvent(SVFBugEvent::SourceInst, getSrcCSID(slice->getSource()))
            };
            report.addSaberBug(GenericBug::PARTIALLEAK, eventStack);
            SVFUtil::errs() << "\t\t sink-bypass return at : ( "
                            << bypassRet->getSourceLoc() << " )\n";
            for (ProgSlice::SVFGNodeSetIter it = slice->sinksBegin(), eit = slice->sinksEnd(); it != eit; ++it)
            {
                const SVFGNode* snk = *it;
                SVFUtil::errs() << "\t\t sink at : ( " << getSinkNodeLoc(snk) << " )\n";
            }
        }
    }

    if(Options::ValidateTests())
        testsValidation(slice);
}


/*!
 * Validate test cases for regression test purpose
 */
void LeakChecker::testsValidation(const ProgSlice* slice)
{
    const SVFGNode* source = slice->getSource();
    const CallICFGNode* cs = getSrcCSID(source);
    const FunObjVar* fun = cs->getCalledFunction();
    if(fun==nullptr)
        return;

    validateSuccessTests(source,fun);
    validateExpectedFailureTests(source,fun);
}


void LeakChecker::validateSuccessTests(const SVFGNode* source, const FunObjVar* fun)
{

    const CallICFGNode* cs = getSrcCSID(source);

    bool success = false;

    if(fun->getName() == "SAFEMALLOC")
    {
        if(isAllPathReachable() == true && isSomePathReachable() == true)
            success = true;
    }
    else if(fun->getName() == "NFRMALLOC")
    {
        if(isAllPathReachable() == false && isSomePathReachable() == false)
            success = true;
    }
    else if(fun->getName() == "PLKMALLOC")
    {
        if(isAllPathReachable() == false && isSomePathReachable() == true)
            success = true;
    }
    else if(fun->getName() == "CLKMALLOC")
    {
        if(isAllPathReachable() == false && isSomePathReachable() == false)
            success = true;
    }
    else if(fun->getName() == "NFRLEAKFP" || fun->getName() == "PLKLEAKFP"
            || fun->getName() == "LEAKFN")
    {
        return;
    }
    else
    {
        writeWrnMsg("\t can not validate, check function not found, please put it at the right place!!");
        return;
    }

    std::string funName = source->getFun()->getName();

    if (success)
    {
        outs() << sucMsg("\t SUCCESS :") << funName << " check <src id:" << source->getId()
               << ", cs id:" << (getSrcCSID(source))->valueOnlyToString() << "> at ("
               << cs->getSourceLoc() << ")\n";
    }
    else
    {
        SVFUtil::errs() << errMsg("\t FAILURE :") << funName << " check <src id:" << source->getId()
                        << ", cs id:" << (getSrcCSID(source))->valueOnlyToString() << "> at ("
                        << cs->getSourceLoc() << ")\n";
        assert(false && "test case failed!");
    }
}

void LeakChecker::validateExpectedFailureTests(const SVFGNode* source, const FunObjVar* fun)
{

    const CallICFGNode* cs = getSrcCSID(source);

    bool expectedFailure = false;

    if(fun->getName() == "NFRLEAKFP")
    {
        if(isAllPathReachable() == false && isSomePathReachable() == false)
            expectedFailure = true;
    }
    else if(fun->getName() == "PLKLEAKFP")
    {
        if(isAllPathReachable() == false && isSomePathReachable() == true)
            expectedFailure = true;
    }
    else if(fun->getName() == "LEAKFN")
    {
        if(isAllPathReachable() == true && isSomePathReachable() == true)
            expectedFailure = true;
    }
    else if(fun->getName() == "SAFEMALLOC" || fun->getName() == "NFRMALLOC"
            || fun->getName() == "PLKMALLOC" || fun->getName() == "CLKLEAKFN")
    {
        return;
    }
    else
    {
        writeWrnMsg("\t can not validate, check function not found, please put it at the right place!!");
        return;
    }

    std::string funName = source->getFun()->getName();

    if (expectedFailure)
    {
        outs() << sucMsg("\t EXPECTED-FAILURE :") << funName << " check <src id:" << source->getId()
               << ", cs id:" << (getSrcCSID(source))->valueOnlyToString() << "> at ("
               << cs->getSourceLoc() << ")\n";
    }
    else
    {
        SVFUtil::errs() << errMsg("\t UNEXPECTED FAILURE :") << funName
                        << " check <src id:" << source->getId()
                        << ", cs id:" << (getSrcCSID(source))->valueOnlyToString() << "> at ("
                        << cs->getSourceLoc() << ")\n";
        assert(false && "test case failed!");
    }
}

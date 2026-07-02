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

std::string LeakChecker::s_alertOutDir_;
bool LeakChecker::s_sliceExportConfigured_ = false;

void LeakChecker::setAlertOutputDir(const std::string& path)
{
    s_alertOutDir_ = path;
    s_sliceExportConfigured_ = !path.empty();
}

bool LeakChecker::sliceExportEnabled() const
{
    return s_sliceExportConfigured_;
}

void LeakChecker::prepareSliceCollector()
{
    if (!sliceExportEnabled())
        return;
    sliceCollector_.setAlertOutDir(s_alertOutDir_);
}

const char* LeakChecker::sliceExportGeneratedBy() const
{
    return "SVFmemplus-LeakChecker";
}

void LeakChecker::collectSliceForPending(const SaberPendingReport& pending)
{
    if (!sliceExportEnabled())
        return;

    prepareSliceCollector();
    SaberSlice slice;
    if (sliceCollector_.collectSliceForPending(pending, slice))
        sliceCollector_.addSlice(slice);
}

void LeakChecker::clearPendingReports()
{
    pendingReports_.clear();
    pendingReportKeys_.clear();
}

bool LeakChecker::queuePendingReport(SaberPendingReport&& report, const std::string& dedupKey)
{
    if (!dedupKey.empty() && !pendingReportKeys_.insert(dedupKey).second)
        return false;
    pendingReports_.push_back(std::move(report));
    return true;
}

void LeakChecker::printPendingSinkInfo(const SaberPendingReport& pending) const
{
    if (!pending.sinkBypassReturnLoc.empty())
    {
        SVFUtil::errs() << "\t\t sink-bypass return at : ( "
                        << pending.sinkBypassReturnLoc << " )\n";
    }
    for (const std::string& loc : pending.sinkLocs)
        SVFUtil::errs() << "\t\t sink at : ( " << loc << " )\n";
}

void LeakChecker::flushPendingReports()
{
    const u32_t pendingCount = pendingReports_.size();
    u32_t emitted = 0;
    for (const SaberPendingReport& pending : pendingReports_)
    {
        report.addSaberBug(pending.bugType, pending.eventStack);
        printPendingSinkInfo(pending);
        collectSliceForPending(pending);
        ++emitted;
    }
    clearPendingReports();
    onPendingReportsFlushed(pendingCount, emitted);
}

void LeakChecker::finalize()
{
    flushPendingReports();
    if (sliceExportEnabled())
    {
        if (!sliceCollector_.alertOutDirRef().empty())
        {
            sliceCollector_.writeAlerts(sliceExportGeneratedBy());
            SVFUtil::outs() << "[SaberAlert] exported " << sliceCollector_.size()
                            << " alert(s) to " << sliceCollector_.alertOutDirRef() << "\n";
        }
    }
    SrcSnkDDA::finalize();
}


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
        if((!includeUncalledAllocSources() && cs->getFun()->isUncalledFunction()) || !cs->getType()->isPointerTy())
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
                        if ((includeUncalledAllocSources() || !cs->getFun()->isUncalledFunction()) &&
                                !isExtCall(cs->getBB()->getParent()))
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

static u32_t getICFGSourceLine(const ICFGNode* node)
{
    if (node == nullptr)
        return UINT32_MAX;
    const std::string& sloc = node->getSourceLoc();
    const size_t brace = sloc.find('{');
    const std::string json = (brace != std::string::npos) ? sloc.substr(brace) : sloc;
    std::string file;
    u32_t line = 0;
    if (!SaberSliceExportUtil::parseLocFileLine(json, file, line) || line == 0)
        return UINT32_MAX;
    return line;
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

    auto enqueueICFGNode = [&](FIFOWorkList<const ICFGNode*>& wl, const ICFGNode* icfgNode)
    {
        if (icfgNode != nullptr && icfgNode->getFun() == fun)
            wl.push(icfgNode);
    };

    FIFOWorkList<const ICFGNode*> worklist;
    Set<const ICFGNode*> visited;
    Set<const ICFGNode*> bypassReturns;
    enqueueICFGNode(worklist, srcCS->getRetICFGNode());

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
                bypassReturns.insert(node);
                continue;
            }
        }

        // Hop over intra-procedural calls (e.g. RPC helpers in DeleteFiles) so that
        // early returns after nested calls are still visible to leak detection.
        if (const CallICFGNode* call = SVFUtil::dyn_cast<CallICFGNode>(node))
            enqueueICFGNode(worklist, call->getRetICFGNode());

        for (ICFGNode::const_iterator it = node->OutEdgeBegin(), eit = node->OutEdgeEnd(); it != eit; ++it)
        {
            const ICFGEdge* edge = *it;
            if (SVFUtil::isa<IntraCFGEdge>(edge))
                enqueueICFGNode(worklist, edge->getDstNode());
            else if (SVFUtil::isa<RetCFGEdge>(edge))
                enqueueICFGNode(worklist, edge->getDstNode());
        }
    }

    const SVFBasicBlock* allocBB = srcCS->getBB();
    SaberCondAllocator* condAllocator = getSaberCondAllocator();

    const ICFGNode* bestBypass = nullptr;
    u32_t bestLine = UINT32_MAX;
    for (const ICFGNode* retNode : bypassReturns)
    {
        const u32_t retLine = getICFGSourceLine(retNode);
        const SVFBasicBlock* retBB = retNode->getBB();
        // Success-path function exits that are not dominated by the allocation site
        // (e.g. disk_cache Walk after a closedir-failure alloc) are not leak paths.
        if (allocBB != nullptr && retBB != nullptr && condAllocator != nullptr &&
                !condAllocator->dominate(allocBB, retBB))
            continue;
        if (retLine < bestLine)
        {
            bestLine = retLine;
            bestBypass = retNode;
        }
    }

    if (bestBypass == nullptr)
        return false;

    bypassRet = bestBypass;
    return true;
}

void LeakChecker::reportBug(ProgSlice* slice)
{
    auto emitBug = [&](GenericBug::BugType bugType, GenericBug::EventStack eventStack,
                       const std::vector<std::string>& sinkLocs = {},
                       const std::string& bypassReturnLoc = {}) {
        SaberPendingReport pending;
        pending.bugType = bugType;
        pending.eventStack = std::move(eventStack);
        pending.sinkLocs = sinkLocs;
        pending.sinkBypassReturnLoc = bypassReturnLoc;
        queuePendingReport(std::move(pending), "");
    };

    if(isAllPathReachable() == false && isSomePathReachable() == false)
    {
        // full leakage
        GenericBug::EventStack eventStack =
        {
            SVFBugEvent(SVFBugEvent::SourceInst, getSrcCSID(slice->getSource()))
        };
        std::vector<std::string> sinkLocs;
        for (ProgSlice::SVFGNodeSetIter it = slice->sinksBegin(), eit = slice->sinksEnd(); it != eit; ++it)
            sinkLocs.push_back(getSinkNodeLoc(*it));
        emitBug(GenericBug::NEVERFREE, eventStack, sinkLocs);
    }
    else if (isAllPathReachable() == false && isSomePathReachable() == true)
    {
        // partial leakage
        GenericBug::EventStack eventStack;
        slice->evalFinalCond2Event(eventStack);
        eventStack.push_back(
            SVFBugEvent(SVFBugEvent::SourceInst, getSrcCSID(slice->getSource())));
        std::vector<std::string> sinkLocs;
        for (ProgSlice::SVFGNodeSetIter it = slice->sinksBegin(), eit = slice->sinksEnd(); it != eit; ++it)
            sinkLocs.push_back(getSinkNodeLoc(*it));
        SaberPendingReport pending;
        pending.bugType = GenericBug::PARTIALLEAK;
        pending.eventStack = std::move(eventStack);
        pending.sinkLocs = sinkLocs;
        for (ProgSlice::SVFGNodeSetIter it = slice->sinksBegin(),
                eit = slice->sinksEnd(); it != eit; ++it)
        {
            GenericBug::EventStack sinkEvents;
            slice->evalSinkCond2Event(*it, sinkEvents);
            pending.sinkPathEvents.push_back(std::move(sinkEvents));
        }
        queuePendingReport(std::move(pending), "");
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
            std::vector<std::string> sinkLocs;
            for (ProgSlice::SVFGNodeSetIter it = slice->sinksBegin(), eit = slice->sinksEnd(); it != eit; ++it)
                sinkLocs.push_back(getSinkNodeLoc(*it));
            SaberPendingReport pending;
            pending.bugType = GenericBug::PARTIALLEAK;
            pending.eventStack = std::move(eventStack);
            pending.sinkLocs = sinkLocs;
            pending.sinkBypassReturnLoc = bypassRet->getSourceLoc();
            for (ProgSlice::SVFGNodeSetIter it = slice->sinksBegin(),
                    eit = slice->sinksEnd(); it != eit; ++it)
            {
                GenericBug::EventStack sinkEvents;
                slice->evalSinkCond2Event(*it, sinkEvents);
                pending.sinkPathEvents.push_back(std::move(sinkEvents));
            }
            queuePendingReport(std::move(pending), "");
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

// svf/lib/SABER/UseAfterFreeChecker.cpp
#include "SABER/UseAfterFreeChecker.h"
#include "SABER/SaberCheckerAPI.h"
#include "SVFIR/SVFIR.h"
// #include "MSSA/SVFG.h"
#include "Util/SVFUtil.h"

using namespace SVF;
using namespace SVFUtil;

/*!
 * Initialize sources
 */
void UseAfterFreeChecker::initSrcs()
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
                addToSources(src);
                addSrcToCSID(src, it->first);

            }
        }
    }

}

/*!
 * Initialize sinks
 */
void UseAfterFreeChecker::initSnks()
{
    SVFIR* pag = getPAG();

    for (SVFIR::iterator it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        SVFVar* var = it->second;
        if(!var->isPointer())
            continue;

        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Load))
        {   
            addToSinks(svfg->getStmtVFGNode(ld));
        }
        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Store))
        {
            addToSinks(getSVFG()->getStmtVFGNode(ld));
        }
        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Call))
        {
            addToSinks(getSVFG()->getStmtVFGNode(ld));
        }
        for(const SVFStmt* ld : var->getOutgoingEdges(SVFStmt::Gep))
        {
            addToSinks(getSVFG()->getStmtVFGNode(ld));
        }
            
    }
}

void UseAfterFreeChecker::reportBug(ProgSlice* slice)
{

    if(isSomePathReachable())
    {
        GenericBug::EventStack eventStack;
        slice->evalFinalCond2Event(eventStack);
        eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, getSrcCSID(slice->getSource())));
        report.addSaberBug(GenericBug::USEAFTERFREE, eventStack);
    }
}
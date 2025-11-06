#include "GraphReaderSVFGBuilder.h"
#include "Graphs/SVFG.h"
#include "MemoryModel/PointerAnalysisImpl.h"
#include "Util/Options.h"

using namespace SVF;
using namespace SVFUtil;

void GraphReaderSVFGBuilder::buildSVFG()
{
    // Build the standard SVFG structure
    SVFGBuilder::buildSVFG();

    // Optionally apply Saber-inspired optimizations
    if (enableSaberOptimizations)
    {
        MemSSA* mssa = svfg->getMSSA();
        BVDataPTAImpl* pta = mssa->getPTA();
        
        DBOUT(DGENERAL, outs() << pasMsg("\t[GraphReader] Applying Saber optimizations\n"));
        applySaberOptimizations(pta);
    }
}

void GraphReaderSVFGBuilder::applySaberOptimizations(BVDataPTAImpl* pta)
{
    // Apply safe optimizations that don't require SaberCondAllocator
    
    DBOUT(DGENERAL, outs() << pasMsg("\t[GraphReader] Collect Global Variables\n"));
    collectGlobals(pta);
    
    DBOUT(DGENERAL, outs() << pasMsg("\t[GraphReader] Remove Dereference Direct SVFG Edge\n"));
    rmDerefDirSVFGEdges(pta);
    
    // Note: We skip rmIncomingEdgeForSUStore and AddExtActualParmSVFGNodes
    // because they require SaberCondAllocator or are specific to memory leak detection
}

void GraphReaderSVFGBuilder::collectGlobals(BVDataPTAImpl* pta)
{
    // Adapted from SaberSVFGBuilder::collectGlobals
    SVFIR* pag = svfg->getPAG();
    NodeVector worklist;
    
    for(SVFIR::iterator it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        PAGNode* pagNode = it->second;
        if (SVFUtil::isa<DummyValVar, DummyObjVar>(pagNode))
            continue;

        if(GepObjVar* gepobj = SVFUtil::dyn_cast<GepObjVar>(pagNode))
        {
            if(SVFUtil::isa<DummyObjVar>(pag->getGNode(gepobj->getBaseNode())))
                continue;
        }
        
        if ((isa<ObjVar>(pagNode) && isa<GlobalObjVar>(pag->getBaseObject(pagNode->getId()))) ||
                (isa<ValVar>(pagNode) && isa<GlobalValVar>(pag->getBaseValVar(pagNode->getId()))))
        {
            worklist.push_back(it->first);
        }
    }

    while(!worklist.empty())
    {
        NodeID id = worklist.back();
        worklist.pop_back();
        globs.set(id);
        
        const PointsTo& pts = pta->getPts(id);
        for(PointsTo::iterator it = pts.begin(), eit = pts.end(); it != eit; ++it)
        {
            if(!globs.test(*it))
                worklist.push_back(*it);
        }
    }
}

void GraphReaderSVFGBuilder::rmDerefDirSVFGEdges(BVDataPTAImpl* pta)
{
    // Adapted from SaberSVFGBuilder::rmDerefDirSVFGEdges
    for(SVFG::iterator it = svfg->begin(), eit = svfg->end(); it != eit; ++it)
    {
        const SVFGNode* node = it->second;

        if(const StmtSVFGNode* stmtNode = SVFUtil::dyn_cast<StmtSVFGNode>(node))
        {
            /// for store, remove edge from RHS/LHS pointer def to store node
            if(SVFUtil::isa<StoreSVFGNode>(stmtNode))
            {
                const SVFGNode* def = svfg->getDefSVFGNode(stmtNode->getPAGDstNode());
                if(SVFGEdge* edge = svfg->getIntraVFGEdge(def, stmtNode, SVFGEdge::IntraDirectVF))
                {
                    svfg->removeSVFGEdge(edge);
                }
            }
            /// for load, remove edge from source pointer def to load node
            else if(SVFUtil::isa<LoadSVFGNode>(stmtNode))
            {
                const SVFGNode* def = svfg->getDefSVFGNode(stmtNode->getPAGSrcNode());
                if(SVFGEdge* edge = svfg->getIntraVFGEdge(def, stmtNode, SVFGEdge::IntraDirectVF))
                {
                    svfg->removeSVFGEdge(edge);
                }
            }
        }
    }
}


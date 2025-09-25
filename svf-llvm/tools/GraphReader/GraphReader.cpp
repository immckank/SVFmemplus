#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"
#include "MSSA/MemSSA.h"
#include "Graphs/SVFG.h"
#include "Graphs/CallGraph.h"
#include "Graphs/ICFG.h"
#include "Util/CDGBuilder.h"

using namespace SVF;
using namespace SVFUtil;

/*!
    // GraphReader: A tool to read and analyze SVF graphs.
 */
int main(int argc, char ** argv) {
    // 解析命令行参数
    std::vector<std::string> moduleNameVec;
    moduleNameVec = OptionBase::parseOptions(
                        argc, argv, "GraphReader", "[options] <input-bitcode...>"
                    );

    outs() << pasMsg("GraphReader Tool Started\n");
    outs() << "================================================================\n";

    LLVMModuleSet::buildSVFModule(moduleNameVec);

    // 构建程序赋值图 (PAG)
    // class: SVFIR
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    outs() << "Step 1: SVFIR (PAG) built.\n";
    outs() << "  - Total PAG Nodes: " << pag->getPAGNodeNum() << "\n";
    outs() << "  - Total PAG Edges: " << pag->getPAGEdgeNum() << "\n";
    outs() << "----------------------------------------------------------------\n";


    // 访问 ICFG
    const ICFG* icfg = pag->getICFG();
    outs() << "Step 2: Accessing ICFG and CallGraph.\n";
    outs() << "  - Total ICFG Nodes: " << icfg->getTotalNodeNum() << "\n";

    // 访问 CallGraph
    const CallGraph* callgraph = pag->getCallGraph();
    outs() << "  - Total CallGraph Nodes: " << callgraph->getTotalNodeNum() << "\n";
    outs() << "----------------------------------------------------------------\n";

    LLVMModuleSet::releaseLLVMModuleSet();
    return 0;
}

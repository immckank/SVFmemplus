#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"
#include "MSSA/MemSSA.h"
#include "Graphs/SVFG.h"
#include "Graphs/CallGraph.h"
#include "Graphs/ICFG.h"
#include "Util/CDGBuilder.h"

using namespace llvm;
using namespace SVF;
using namespace SVFUtil;

/*!
 * 遍历从 iNode 到 targetNode 的所有路径，并打印路径上的所有条件分支边。
 */
void pathConditionReader(ICFG* icfg, const ICFGNode* iNode, const ICFGNode* targetNode)
{
    // 调用栈
    // 假设函数 A 和 B 都调用了函数 F。从 A 进入 F，然后从 F 返回到 B。但是这在实际程序中是不可能发生的。
    using CallStack = std::vector<const RetICFGNode*>;
    // 模拟栈进行深搜
    // {当前节点, 到达此节点的路径上的分支边, 路径上已访问的节点集合, 调用栈}
    // 避免陷入循环
    std::vector<std::tuple<const ICFGNode*, std::vector<const IntraCFGEdge*>, Set<const ICFGNode*>, CallStack>> worklist;

    worklist.emplace_back(iNode, std::vector<const IntraCFGEdge*>{}, Set<const ICFGNode*>{iNode}, CallStack{});

    SVF::SVFUtil::outs() << "Starting traversal from Node " << iNode->getId() << " to " << targetNode->getId() << "\n";

    while (!worklist.empty())
    {
        auto [currentNode, currentPathEdges, pathVisited, callStack] = worklist.back();
        worklist.pop_back();

        // // Debug 输出当前节点
        // SVF::SVFUtil::outs() << "  Visiting Node " << currentNode->getId() << "\n";
        // // Debug 输出当前worklist长度
        // SVF::SVFUtil::outs() << "  Current worklist size: " << worklist.size() << "\n";

        // 深搜终点找到目标节点
        if (currentNode == targetNode) {
            SVF::SVFUtil::outs() << "  Found a path to target node " << targetNode->getId() << ". Conditional branches on this path:\n";
            if (currentPathEdges.empty()) {
                SVF::SVFUtil::outs() << "    - No conditional branches on this path.\n";
            } else {
                for (const auto* branchEdge : currentPathEdges) {
                    SVF::SVFUtil::outs() << "    - Branch from " << branchEdge->getSrcNode()->getId()
                                         << " to " << branchEdge->getDstNode()->getId()
                                         << " (Condition: " << branchEdge->getCondition()->toString()
                                         << ", Value: " << branchEdge->getSuccessorCondValue() << ")\n";
                }
            }
            // 找到一条路径后继续寻找其他路径 or 直接结束？
            continue; 
        }
        // 终止节点直接返回
        if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(currentNode); callNode && SVFUtil::isProgExitCall(callNode)) {
            SVF::SVFUtil::outs() << "  Path terminated at program exit call: Node " << callNode->getId() << "\n";
            continue;
        }
        // 遍历当前节点的所有出边（包括调用边）
        for (ICFGEdge* edge : currentNode->getOutEdges())
        {
            ICFGNode* succNode = edge->getDstNode();
            // 避免循环。
            if (pathVisited.count(succNode)) {
                continue;
            }
            auto newPathEdges = currentPathEdges;
            auto newPathVisited = pathVisited;
            auto newCallStack = callStack;
            newPathVisited.insert(succNode);
            if (const CallCFGEdge* callEdge = SVFUtil::dyn_cast<CallCFGEdge>(edge)) {
                // 函数调用前压入返回点
                const CallICFGNode* callSiteNode = callEdge->getCallSite();
                newCallStack.push_back(callSiteNode->getRetICFGNode());
            }
            else if (SVFUtil::isa<RetCFGEdge>(edge)) {
                // 函数返回
                const RetICFGNode* retSiteNode = SVFUtil::cast<RetICFGNode>(succNode);
                if (newCallStack.empty() || newCallStack.back() != retSiteNode) {
                    continue;
                }
                newCallStack.pop_back();
            }
            else if (const IntraCFGEdge* intraEdge = SVFUtil::dyn_cast<IntraCFGEdge>(edge)) {
                if (intraEdge->getCondition()) {
                    newPathEdges.push_back(intraEdge);
                }
            }
            worklist.emplace_back(succNode, newPathEdges, newPathVisited, newCallStack);
        }
    }
}

/*!
 * \brief 根据源代码位置字符串查找ICFGNode。
 *
 * 遍历ICFG中的所有节点，匹配文件名和行号。
 *
 * \param icfg 指向ICFG的指针。
 * \param location 形如 "filename:line" 的字符串。
 * \return 如果找到，则为匹配的ICFGNode指针，否则为nullptr。
 */
const ICFGNode* findICFGNodeByLocation(const ICFG* icfg, const std::string& location) {
    size_t colon_pos = location.find(':');
    if (colon_pos == std::string::npos) {
        SVF::SVFUtil::outs() << "Invalid location format. Expected 'filename:line'.\n";
        return nullptr;
    }
    std::string target_filename = location.substr(0, colon_pos);
    std::string target_line_str = location.substr(colon_pos + 1);
    for (ICFG::const_iterator it = icfg->begin(), eit = icfg->end(); it != eit; ++it) {
        const ICFGNode* node = it->second;
        if (node) {
            std::string sourceLoc = node->getSourceLoc();
            std::string file_pattern = "\"" + target_filename + "\"";
            std::string line_pattern = "\"ln\": " + target_line_str;

            if (sourceLoc.find(file_pattern) != std::string::npos &&
                sourceLoc.find(line_pattern) != std::string::npos) {
                // OUT: 打印结果
                SVF::SVFUtil::outs() << "Found matching ICFGNode (ID: " << node->getId() << ") at location: " << sourceLoc << "\n";
                return node;
            }
        }
    }
    SVF::SVFUtil::outs() << "Could not find ICFGNode for location: " << location << "\n";
    return nullptr;
}

/*!
    // GraphReader: A tool to read and analyze SVF graphs.
 */
int main(int argc, char ** argv) {
    // 解析命令行参数
    std::vector<std::string> moduleNameVec;
    moduleNameVec = OptionBase::parseOptions(
                        argc, argv, "GraphReader", "[options] <input-bitcode...>"
                    );

    SVF::SVFUtil::outs() << pasMsg("GraphReader Tool Started\n");
    SVF::SVFUtil::outs() << "================================================================\n";

    LLVMModuleSet::buildSVFModule(moduleNameVec);

    // 构建程序赋值图 (PAG)
    // class: SVFIR
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    SVF::SVFUtil::outs() << "Step 1: SVFIR (PAG) built.\n";
    SVF::SVFUtil::outs() << "  - Total PAG Nodes: " << pag->getPAGNodeNum() << "\n";
    SVF::SVFUtil::outs() << "  - Total PAG Edges: " << pag->getPAGEdgeNum() << "\n";
    SVF::SVFUtil::outs() << "----------------------------------------------------------------\n";

    // 访问 ICFG
    ICFG* icfg = pag->getICFG();
    SVF::SVFUtil::outs() << "Step 2: Accessing ICFG and CallGraph.\n";
    SVF::SVFUtil::outs() << "  - Total ICFG Nodes: " << icfg->getTotalNodeNum() << "\n";

    ICFGNode* startNode = const_cast<ICFGNode*>(
        findICFGNodeByLocation(icfg, "restart.c:76"));

    ICFGNode* targetNode = const_cast<ICFGNode*>(
        findICFGNodeByLocation(icfg, "restart.c:121"));

    if (targetNode) {
        SVF::SVFUtil::outs() << "Traversing ICFG from node ID: " << targetNode->getId() << "\n";
        pathConditionReader(icfg, startNode, targetNode);
    }

    // // 访问 CallGraph
    // const CallGraph* callgraph = pag->getCallGraph();
    // outs() << "  - Total CallGraph Nodes: " << callgraph->getTotalNodeNum() << "\n";
    // outs() << "----------------------------------------------------------------\n";

    LLVMModuleSet::releaseLLVMModuleSet();
    return 0;
}

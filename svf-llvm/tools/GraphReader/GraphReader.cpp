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
 * \brief 遍历从 startNode 到 targetNode 的所有路径，并打印路径上的所有条件分支边。
 *
 * 用栈模拟深搜过程 寻找所有路径 报出路径上的条件分支边和所有调用且未返回的函数调用边
 *
 * \param icfg 指向ICFG的指针。
 * \param startNode 路径搜索的起始ICFG节点。
 * \param targetNode 路径搜索的目标ICFG节点。
 */
void pathCondFuncReader(ICFG* icfg, const ICFGNode* startNode, const ICFGNode* targetNode) 
{
    // 调用栈，用于跟踪函数调用和返回，确保路径的有效性
    using CallStack = std::vector<const RetICFGNode*>;
    // 模拟栈进行深度优先搜索
    // {当前节点, 到达此节点的路径上的分支边, 路径上已访问的节点集合, 调用栈}
    std::vector<std::tuple<const ICFGNode*, std::vector<const IntraCFGEdge*>, Set<const ICFGNode*>, CallStack>> worklist;
    worklist.emplace_back(startNode, std::vector<const IntraCFGEdge*>{}, Set<const ICFGNode*>{startNode}, CallStack{});
    SVF::SVFUtil::outs() << "Starting traversal from Node " << startNode->getId() << " to " << targetNode->getId() << "\n";

    while (!worklist.empty())
    {
        auto [currentNode, currentPathEdges, pathVisited, callStack] = worklist.back();
        worklist.pop_back();

        if (currentNode == targetNode) {
            SVF::SVFUtil::outs() << "  Found a path to target node " << targetNode->getId() << ".\n";
            SVF::SVFUtil::outs() << "  Conditional branches on this path:\n";
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
            SVF::SVFUtil::outs() << "  Unreturned function calls on this path:\n";
            if (callStack.empty()) {
                SVF::SVFUtil::outs() << "    - No unreturned function calls.\n";
            } else {
                for (const auto* retNode : callStack) {
                    SVF::SVFUtil::outs() << "    - Call at Node " << retNode->getCallICFGNode()->getId() << " (" << retNode->getCallICFGNode()->getFun()->getName() << ")\n";
                }
            }
            continue; 
        }

        if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(currentNode); callNode && SVFUtil::isProgExitCall(callNode)) {
            SVF::SVFUtil::outs() << "  Path terminated at program exit call: Node " << callNode->getId() << "\n";
            continue;
        }

        for (ICFGEdge* edge : currentNode->getOutEdges())
        {
            ICFGNode* succNode = edge->getDstNode();
            if (pathVisited.count(succNode)) {
                continue;
            }

            auto newPathEdges = currentPathEdges;
            auto newPathVisited = pathVisited;
            auto newCallStack = callStack;
            newPathVisited.insert(succNode);

            if (const CallCFGEdge* callEdge = SVFUtil::dyn_cast<CallCFGEdge>(edge)) {
                const CallICFGNode* callSiteNode = callEdge->getCallSite();
                newCallStack.push_back(callSiteNode->getRetICFGNode());
            }
            else if (SVFUtil::isa<RetCFGEdge>(edge)) {
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
 * \brief 遍历从 startNode 到 targetNode 的所有路径，并打印路径上的所有条件分支边。
 *
 * 用栈模拟深搜过程 寻找所有路径 报出路径上的条件分支边
 *
 * \param icfg 指向ICFG的指针。
 * \param startNode 路径搜索的起始ICFG节点。
 * \param targetNode 路径搜索的目标ICFG节点。
 */
void pathConditionReader(ICFG* icfg, const ICFGNode* startNode, const ICFGNode* targetNode)
{
    // 调用栈
    // 假设函数 A 和 B 都调用了函数 F。从 A 进入 F，然后从 F 返回到 B。但是这在实际程序中是不可能发生的。
    using CallStack = std::vector<const RetICFGNode*>;
    // 模拟栈进行深搜
    // {当前节点, 到达此节点的路径上的分支边, 路径上已访问的节点集合, 调用栈}
    // 避免陷入循环
    std::vector<std::tuple<const ICFGNode*, std::vector<const IntraCFGEdge*>, Set<const ICFGNode*>, CallStack>> worklist;
    worklist.emplace_back(startNode, std::vector<const IntraCFGEdge*>{}, Set<const ICFGNode*>{startNode}, CallStack{});
    SVF::SVFUtil::outs() << "Starting traversal from Node " << startNode->getId() << " to " << targetNode->getId() << "\n";

    while (!worklist.empty())
    {
        auto [currentNode, currentPathEdges, pathVisited, callStack] = worklist.back();
        worklist.pop_back();
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


/*
整合两个方法 
实现读取string& startlocation和string& targetlocation
返回路径上的条件分支边及相关函数调用边
*/
void pathCondFuncExtractor(ICFG* icfg, const std::string& startLocation, const std::string& targetLocation) {
    const ICFGNode* startNode = findICFGNodeByLocation(icfg, startLocation);
    const ICFGNode* targetNode = findICFGNodeByLocation(icfg, targetLocation);
    if (!startNode || !targetNode) {
        SVF::SVFUtil::outs() << "Invalid start or target location.\n";
        return;
    }
    pathCondFuncReader(icfg, startNode, targetNode);
}

/*!
 * \brief 根据源代码位置查找并打印其所在函数的函数体 (LLVM IR)。
 *
 * 1. 使用 findICFGNodeByLocation 找到与源代码位置匹配的 ICFGNode。
 * 2. 从 ICFGNode 获取其所属的 SVFFunction。
 * 3. 从 SVFFunction 获取底层的 llvm::Function。
 * 4. 打印 llvm::Function 的内容。
 *
 * \param icfg 指向ICFG的指针。
 * \param location 形如 "filename:line" 的字符串。
 */
void printFunctionBodyByLocation(ICFG* icfg, const std::string& location) {
    SVF::SVFUtil::outs() << "Searching for function body at location: " << location << "\n";
    const ICFGNode* node = findICFGNodeByLocation(icfg, location);
    if (!node) {
        // findICFGNodeByLocation 已经打印了错误信息，这里可以直接返回。
        return;
    }

    const FunObjVar* svfFun = node->getFun();
    if (!svfFun) {
        SVF::SVFUtil::outs() << "Error: ICFGNode " << node->getId() << " is not associated with a function.\n";
        return;
    }

    // 获取 LLVMModuleSet 单例
    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
    // 使用 getLLVMValue 方法将 SVF 的 FunObjVar 转换为 LLVM 的 Value
    const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(svfFun);
    // 将 llvm::Value* 安全地转换为 llvm::Function*
    const llvm::Function* llvmFun = SVFUtil::dyn_cast<llvm::Function>(llvmVal);
    SVF::SVFUtil::outs() << "==================== Function Body for " << svfFun->getName() << " ====================\n";
    llvmFun->print(llvm::outs());
    SVF::SVFUtil::outs() << "\n================================================================================\n";
}

/*!
 * \brief 根据源代码位置查找函数调用，并打印被调用函数的函数体 (LLVM IR)。
 *
 * 1. 使用 findICFGNodeByLocation 找到与源代码位置匹配的 ICFGNode。
 * 2. 检查该节点是否为 CallICFGNode，即一个函数调用点。
 * 3. 遍历该调用点的所有出边，寻找 CallCFGEdge。
 * 4. 对于每个 CallCFGEdge，其目标节点是一个被调用函数的入口 (EntryICFGNode)。
 * 5. 从入口节点获取函数信息，并打印其 LLVM IR 函数体。
 *
 * \param icfg 指向ICFG的指针。
 * \param location 形如 "filename:line" 的字符串，表示函数调用的位置。
 */
void printCalleeFunctionBodyByLocation(ICFG* icfg, const std::string& location) {
    SVF::SVFUtil::outs() << "Searching for callee function body at call site location: " << location << "\n";
    const ICFGNode* node = findICFGNodeByLocation(icfg, location);
    if (!node) {
        return;
    }

    const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node);
    if (!callNode) {
        SVF::SVFUtil::outs() << "Error: Node " << node->getId() << " at location " << location << " is not a function call site.\n";
        return;
    }

    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
    int calleeCount = 0;

    for (ICFGEdge* edge : callNode->getOutEdges()) {
        if (SVFUtil::isa<CallCFGEdge>(edge)) {
            const ICFGNode* calleeEntryNode = edge->getDstNode();
            const FunObjVar* svfFun = calleeEntryNode->getFun();
            if (svfFun) {
                calleeCount++;
                const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(svfFun);
                const llvm::Function* llvmFun = SVFUtil::dyn_cast<llvm::Function>(llvmVal);
                SVF::SVFUtil::outs() << "==================== Callee Function Body for " << svfFun->getName() << " ====================\n";
                llvmFun->print(llvm::outs());
                SVF::SVFUtil::outs() << "\n================================================================================\n";
            }
        }
    }
    if (calleeCount == 0) {
        SVF::SVFUtil::outs() << "No callees found for the call site at Node " << callNode->getId() << ".\n";
    }
}

/*!
 * \brief 根据函数名查找并打印其所有被调用的位置。
 *
 * 1. 遍历ICFG中的所有节点。
 * 2. 筛选出 CallICFGNode 类型的节点，即函数调用点。
 * 3. 对于每个调用点，遍历其出边，找到代表函数调用的 CallCFGEdge。
 * 4. 获取被调用函数的入口节点，并从中得到函数名。
 * 5. 如果函数名与目标函数名匹配，则打印该调用点的源代码位置。
 *
 * \param icfg 指向ICFG的指针。
 * \param functionName 要查找的函数名。
 */
void printFunctionCallSites(ICFG* icfg, const std::string& functionName) {
    SVF::SVFUtil::outs() << "Searching for all call sites of function: '" << functionName << "'\n";
    int callSiteCount = 0;
    // 使用集合来避免因多条调用边指向同一函数而重复打印同一调用点
    Set<const ICFGNode*> reportedCallSites;

    for (ICFG::const_iterator it = icfg->begin(), eit = icfg->end(); it != eit; ++it) {
        const ICFGNode* node = it->second;
        if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node)) {
            for (ICFGEdge* edge : callNode->getOutEdges()) {
                if (SVFUtil::isa<CallCFGEdge>(edge)) {
                    const ICFGNode* calleeEntryNode = edge->getDstNode();
                    const FunObjVar* svfFun = calleeEntryNode->getFun();
                    if (svfFun && svfFun->getName() == functionName && reportedCallSites.find(callNode) == reportedCallSites.end()) {
                        SVF::SVFUtil::outs() << "  - Found call at: " << callNode->getSourceLoc() << " (Node ID: " << callNode->getId() << ")\n";
                        reportedCallSites.insert(callNode);
                        callSiteCount++;
                    }
                }
            }
        }
    }
    SVF::SVFUtil::outs() << "Found " << callSiteCount << " call site(s) for function '" << functionName << "'.\n";
    SVF::SVFUtil::outs() << "================================================================================\n";
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

    // 示例1：查找两条路径之间的条件
    // pathCondFuncExtractor(icfg, "restart.c:76", "restart.c:121");

    // 示例2：根据代码行号查找并打印其所在函数的函数体
    // printFunctionBodyByLocation(icfg, "stats_prefix.c:118");

    // 示例3：根据代码行号查找函数调用，并打印被调用函数的函数体
    // printCalleeFunctionBodyByLocation(icfg, "stats_prefix.c:118");

    // 示例4：根据函数名查找其所有被调用的位置
    printFunctionCallSites(icfg, "stats_prefix_record_get");

    LLVMModuleSet::releaseLLVMModuleSet();
    return 0;
}

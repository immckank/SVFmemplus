#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"
#include "MSSA/MemSSA.h"
#include "Graphs/SVFG.h"
#include "Graphs/CallGraph.h"
#include "Graphs/ICFG.h"
#include "Util/CDGBuilder.h"
#include <llvm/IR/DebugInfo.h>
#include <llvm/Support/JSON.h>

using namespace llvm;
using namespace SVF;
using namespace SVFUtil;

/*!
 * \brief 根据源代码位置字符串查找ICFGNode。
 *
 * 遍历ICFG中的所有节点，匹配文件名和行号。
 *
 * \param icfg 指向ICFG的指针。
 * \param location 形如 "filename:line" 的字符串。
 * \return 如果找到，则为匹配的ICFGNode指针，否则为nullptr。
 */
// Tool Function
const ICFGNode* findICFGNodeByLocation(const ICFG* icfg, const std::string& location) {
    size_t colon_pos = location.find(':');
    if (colon_pos == std::string::npos) {
        //SVF::SVFUtil::outs() << "Invalid location format. Expected 'filename:line'.\n";
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
                //SVF::SVFUtil::outs() << "Found matching ICFGNode (ID: " << node->getId() << ") at location: " << sourceLoc << "\n";
                return node;
            }
        }
    }
    //SVF::SVFUtil::outs() << "Could not find ICFGNode for location: " << location << "\n";
    return nullptr;
}

/*!
 * \brief 获取一个 LLVM 函数的起始和结束行号。
 *
 * 1. 通过函数的 DISubprogram 获取起始行号。
 * 2. 遍历函数内的所有指令，查找最大的行号作为结束行号。
 *
 * \param llvmFun 指向 llvm::Function 的指针。
 * \return 一个包含起始和结束行号的 std::pair<unsigned, unsigned>。如果找不到调试信息，则返回 {0, 0}。
 */
// Tool Function
struct FunctionSourceInfo {
    std::string filename;
    unsigned startLine;
    unsigned endLine;
};

FunctionSourceInfo getFunctionSourceInfo(const llvm::Function* llvmFun) {
    if (!llvmFun) {
        return {"", 0, 0};
    }

    // 获取函数的调试信息子程序
    llvm::DISubprogram* disub = llvmFun->getSubprogram();
    if (!disub) {
        // 如果没有调试信息，无法确定行号
        return {"", 0, 0};
    }

    std::string filename = disub->getFilename().str();
    unsigned startLine = disub->getLine();
    unsigned endLine = startLine;

    // 遍历函数中的所有指令以找到最大行号
    for (const auto& bb : *llvmFun) {
        for (const auto& inst : bb) {
            const llvm::DebugLoc& loc = inst.getDebugLoc();
            if (loc && loc.getLine() > endLine) {
                endLine = loc.getLine();
            }
        }
    }
    return {filename, startLine, endLine};
}

// 一个辅助函数，用于从主字符串中提取特定键的值
// Tool Function
std::string extract_value(const std::string& source, const std::string& key) {
    // 查找键，例如查找 "\"fl\": \""
    std::string search_key = "\"" + key + "\": ";
    size_t start_pos = source.find(search_key);

    if (start_pos == std::string::npos) {
        throw std::runtime_error("Key '" + key + "' not found.");
    }

    // 值的起始位置在 key 之后
    start_pos += search_key.length();

    // 如果值是字符串，它被双引号包围
    if (source[start_pos] == '"') {
        start_pos++; // 跳过起始引号
        size_t end_pos = source.find('"', start_pos);
        if (end_pos == std::string::npos) {
            throw std::runtime_error("Malformed string: closing quote not found for key '" + key + "'.");
        }
        return source.substr(start_pos, end_pos - start_pos);
    } 
    // 如果值是数字，它后面跟着逗号或花括号
    else {
        size_t end_pos = source.find_first_of(",}", start_pos);
        if (end_pos == std::string::npos) {
             throw std::runtime_error("Malformed string: terminator not found for key '" + key + "'.");
        }
        return source.substr(start_pos, end_pos - start_pos);
    }
}

/*!
 * \brief 遍历从 startNode 到 targetNode 的所有路径，并打印路径上的所有条件分支边。
 *
 * 用栈模拟深搜过程 寻找所有路径 报出路径上的条件分支边和所有调用且未返回的函数调用边
 *
 * \param icfg 指向ICFG的指针。
 * \param startLocation 路径搜索的起始位置。
 * \param targetLocation 路径搜索的目标位置。
 */
void pathCondFuncReader(ICFG* icfg, const std::string& startLocation, const std::string& targetLocation) 
{
    llvm::json::Object result;
    llvm::json::Array pathsArray;

    const ICFGNode* startNode = findICFGNodeByLocation(icfg, startLocation);
    const ICFGNode* targetNode = findICFGNodeByLocation(icfg, targetLocation);
    if (!startNode || !targetNode) {
        result["error"] = true;
        result["message"] = "Invalid start or target location.";
        llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
        return;
    }
    // 调用栈，用于跟踪函数调用和返回，确保路径的有效性
    using CallStack = std::vector<const RetICFGNode*>;
    // 模拟栈进行深度优先搜索
    // {当前节点, 到达此节点的路径上的分支边, 路径上已访问的节点集合, 调用栈}
    std::vector<std::tuple<const ICFGNode*, llvm::json::Array, Set<const ICFGNode*>, CallStack>> worklist;
    worklist.emplace_back(startNode, llvm::json::Array{}, Set<const ICFGNode*>{startNode}, CallStack{});
    
    int pathIdCounter = 0;

    while (!worklist.empty())
    {
        auto [currentNode, currentPathEvents, pathVisited, callStack] = worklist.back();
        worklist.pop_back();

        if (currentNode == targetNode) {
            pathIdCounter++;
            llvm::json::Object pathObject;
            pathObject["path_id"] = pathIdCounter;

            llvm::json::Array finalPathEvents = currentPathEvents;
            // 添加未返回的函数调用到事件列表
            for (const auto* retNode : callStack) {
                const CallICFGNode* callSiteNode = retNode->getCallICFGNode();
                std::string locString = callSiteNode->getSourceLoc();
                std::string formattedLoc = locString;
                try {
                    std::string file = extract_value(locString, "fl");
                    std::string line = extract_value(locString, "ln");
                    formattedLoc = file + ":" + line;
                } catch (const std::runtime_error&) {}

                finalPathEvents.push_back(llvm::json::Object{
                    {"type", "unreturned-call"},
                    {"location", formattedLoc},
                    {"function", callSiteNode->getFun()->getName()}
                });
            }

            pathObject["events"] = std::move(finalPathEvents);
            pathsArray.push_back(std::move(pathObject));
            continue; 
        }

        if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(currentNode); callNode && SVFUtil::isProgExitCall(callNode)) {
            continue;
        }

        for (ICFGEdge* edge : currentNode->getOutEdges())
        {
            ICFGNode* succNode = edge->getDstNode();
            if (pathVisited.count(succNode)) {
                continue;
            }

            auto newPathEvents = currentPathEvents;
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
                    std::string locString = intraEdge->getSrcNode()->getSourceLoc();
                    std::string formattedLoc = locString;
                    try {
                        std::string file = extract_value(locString, "fl");
                        std::string line = extract_value(locString, "ln");
                        formattedLoc = file + ":" + line;
                    } catch (const std::runtime_error&) {}

                    newPathEvents.push_back(llvm::json::Object{
                        {"type", "branch"},
                        {"location", formattedLoc},
                        {"condition_value", intraEdge->getSuccessorCondValue() == 1 ? "true" : "false"}
                    });
                }
            }
            worklist.emplace_back(succNode, std::move(newPathEvents), std::move(newPathVisited), std::move(newCallStack));
        }
    }

    result["paths"] = std::move(pathsArray);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
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
// TODO: 设计输出格式
void pathCondReader(ICFG* icfg, const std::string& startLocation, const std::string& targetLocation)
{
    const ICFGNode* startNode = findICFGNodeByLocation(icfg, startLocation);
    const ICFGNode* targetNode = findICFGNodeByLocation(icfg, targetLocation);
    if (!startNode || !targetNode) {
        SVF::SVFUtil::outs() << "Invalid start or target location.\n";
        return;
    }
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
 * \brief 根据源代码位置查找并打印其所在函数的函数体 (LLVM IR)。
 *
 * 1. 使用 findICFGNodeByLocation 找到与源代码位置匹配的 ICFGNode。
 * 2. 从 ICFGNode 获取其所属的 SVFFunction。
 * 3. 从 SVFFunction 获取底层的 llvm::Function。
 *
 * \param icfg 指向ICFG的指针。
 * \param location 形如 "filename:line" 的字符串。
 */
void printFunctionBodyByLocation(ICFG* icfg, const std::string& location) {
    llvm::json::Object result;

    const ICFGNode* node = findICFGNodeByLocation(icfg, location);
    if (!node) {
        result["error"] = true;
        result["message"] = "Could not find ICFGNode for the given location.";
        llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
        return;
    }

    const FunObjVar* svfFun = node->getFun();
    if (!svfFun) {
        result["error"] = true;
        result["message"] = "The ICFGNode at the given location is not inside a function.";
        llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
        return;
    }

    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
    const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(svfFun);
    const llvm::Function* llvmFun = SVFUtil::dyn_cast<llvm::Function>(llvmVal);

    FunctionSourceInfo sourceInfo = getFunctionSourceInfo(llvmFun);

    result["function_name"] = svfFun->getName();
    result["filename"] = sourceInfo.filename;
    result["start_line"] = sourceInfo.startLine;
    result["end_line"] = sourceInfo.endLine;
    result["error"] = false;

    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
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
void printCalleeFunctionBodyByLocation(ICFG* icfg, const std::string& location)
{
    llvm::json::Object result;
    llvm::json::Array calleeFunctions;

    const ICFGNode* node = findICFGNodeByLocation(icfg, location);
    if (!node) {
        result["error"] = true;
        result["message"] = "Could not find ICFGNode for the given location.";
        llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
        return;
    }

    const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node);
    if (!callNode) {
        result["error"] = true;
        result["message"] = "Node at the given location is not a function call site.";
        llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
        return;
    }

    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

    for (ICFGEdge* edge : callNode->getOutEdges()) {
        if (SVFUtil::isa<CallCFGEdge>(edge)) {
            const ICFGNode* calleeEntryNode = edge->getDstNode();
            const FunObjVar* svfFun = calleeEntryNode->getFun();
            if (svfFun) {
                const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(svfFun);
                const llvm::Function* llvmFun = SVFUtil::dyn_cast<llvm::Function>(llvmVal);

                FunctionSourceInfo sourceInfo = getFunctionSourceInfo(llvmFun);
                calleeFunctions.push_back(llvm::json::Object{
                    {"function_name", svfFun->getName()},
                    {"filename", sourceInfo.filename},
                    {"start_line", sourceInfo.startLine},
                    {"end_line", sourceInfo.endLine}
                });
            }
        }
    }

    result["callee_functions"] = std::move(calleeFunctions);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
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
    llvm::json::Object result;
    llvm::json::Array callSites;

    // 使用集合来避免因多条调用边指向同一函数而重复打印同一调用点
    Set<const ICFGNode*> reportedCallSites;

    for (ICFG::const_iterator it = icfg->begin(), eit = icfg->end(); it != eit; ++it) {
        const ICFGNode* node = it->second;
        if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node)) {
            // 如果已经报告过这个调用点，则跳过
            if (reportedCallSites.count(callNode)) {
                continue;
            }
            for (ICFGEdge* edge : callNode->getOutEdges()) {
                if (SVFUtil::isa<CallCFGEdge>(edge)) {
                    const ICFGNode* calleeEntryNode = edge->getDstNode();
                    const FunObjVar* svfFun = calleeEntryNode->getFun();
                    if (svfFun && svfFun->getName() == functionName) {
                        llvm::json::Object site;
                        std::string locString = callNode->getSourceLoc();
                        std::string formattedLoc = locString; // 默认值
                        try {
                            std::string file = extract_value(locString, "fl");
                            std::string line = extract_value(locString, "ln");
                            formattedLoc = file + ":" + line;
                        } catch (const std::runtime_error& e) {
                            // 如果提取失败，保持默认值
                        }
                        site["location"] = formattedLoc;
                        callSites.push_back(std::move(site));
                        reportedCallSites.insert(callNode);
                        break; // 找到匹配后，处理下一个CallNode
                    }
                }
            }
        }
    }

    result["call_sites"] = std::move(callSites);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
}


/*!
 * GraphReader: A tool to read and analyze SVF graphs.
 * It uses SVF's command-line option system for analysis selection.
 */
int main(int argc, char ** argv) {
    // 解析命令行参数，这会填充上面定义的 cl::opt 变量
    // 并将非选项参数（即 bitcode 文件）收集到 moduleNameVec 中
    std::vector<std::string> moduleNameVec = 
        OptionBase::parseOptions(argc, argv, "GraphReader", "[options] <input-bitcode...>");

    // SVF::SVFUtil::outs() << pasMsg("GraphReader Tool Started\n");
    // SVF::SVFUtil::outs() << "================================================================\n";

    LLVMModuleSet::buildSVFModule(moduleNameVec);

    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    // SVF::SVFUtil::outs() << "SVFIR (PAG) built.\n";

    ICFG* icfg = pag->getICFG();
    if (!Options::FindCallSites().empty()) {
        printFunctionCallSites(icfg, Options::FindCallSites());
    }
    else if (!Options::FindCalleeBody().empty()) {
        printCalleeFunctionBodyByLocation(icfg, Options::FindCalleeBody());
    }
    else if (!Options::FindFuncBody().empty()) {
        printFunctionBodyByLocation(icfg, Options::FindFuncBody());
    }
    else if (!Options::PathCondFuncStart().empty() && !Options::PathCondFuncEnd().empty()) {
        pathCondFuncReader(icfg, Options::PathCondFuncStart(), Options::PathCondFuncEnd());
    }
    else {
        SVF::SVFUtil::outs() << "No analysis option specified. Use --help to see available options.\n";
        SVF::SVFUtil::outs() << "Defaulting to an example: finding call sites for 'stats_prefix_record_get'\n";
        printFunctionCallSites(icfg, "stats_prefix_record_get");
    }
    return 0;
}

// // DeBug main
// int main(int argc, char** argv) {
//     // 解析命令行参数
//     std::vector<std::string> moduleNameVec =
//         OptionBase::parseOptions(argc, argv, "GraphReader", "[options] <input-bitcode...>");
    
//     LLVMModuleSet::buildSVFModule(moduleNameVec);
//     SVFIRBuilder builder;
//     SVFIR* pag = builder.build();
//     ICFG* icfg = pag->getICFG();

//     // 示例1：查找两条路径之间的条件
//     pathCondFuncReader(icfg, "restart.c:76", "restart.c:121");

//     // 示例2：根据代码行号查找并打印其所在函数的函数体
//     // printFunctionBodyByLocation(icfg, "stats_prefix.c:118");

//     // 示例3：根据代码行号查找函数调用，并打印被调用函数的函数体
//     // printCalleeFunctionBodyByLocation(icfg, "stats_prefix.c:118");

//     // 示例4：根据函数名查找其所有被调用的位置
//     // printFunctionCallSites(icfg, "stats_prefix_record_get");

//     return 0;
// }

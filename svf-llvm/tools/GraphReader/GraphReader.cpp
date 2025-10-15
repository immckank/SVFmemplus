#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVFIR/SVFValue.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"
#include "MSSA/MemSSA.h"
#include "SABER/SaberSVFGBuilder.h"
#include "Graphs/SVFG.h"
#include "Graphs/CallGraph.h"
#include "Graphs/ICFG.h"
#include "Util/CDGBuilder.h"
// #include "GraphReader/SVFGChecker.h"
#include "GraphReaderUtil.h"
#include <llvm/IR/DebugInfo.h>
#include <llvm/Support/JSON.h>

using namespace llvm;
using namespace SVF;
using namespace SVFUtil;

/*!
 * \brief 遍历从 startNode 到 targetNode 的所有路径，并输出路径上的所有条件分支边及调用未返回的函数。
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

    const ICFGNode* startNode = GraphReaderUtil::findICFGNodeByLocation(icfg, startLocation);
    const ICFGNode* targetNode = GraphReaderUtil::findICFGNodeByLocation(icfg, targetLocation);
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
                std::string formattedLoc = "unknown";
                
                llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(locString);
                if (auto file = locInfo.getString("fl")) {
                    if (auto line = locInfo.getInteger("ln")) {
                        formattedLoc = file->str() + ":" + std::to_string(*line);
                    }
                }

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
                    std::string formattedLoc = "unknown";
                    
                    llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(locString);
                    if (auto file = locInfo.getString("fl")) {
                        if (auto line = locInfo.getInteger("ln")) {
                            formattedLoc = file->str() + ":" + std::to_string(*line);
                        }
                    }
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
    const ICFGNode* startNode = GraphReaderUtil::findICFGNodeByLocation(icfg, startLocation);
    const ICFGNode* targetNode = GraphReaderUtil::findICFGNodeByLocation(icfg, targetLocation);
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

    const ICFGNode* node = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
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

    FunctionSourceInfo sourceInfo = GraphReaderUtil::getFunctionSourceInfo(llvmFun);

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

    const ICFGNode* node = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
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

                FunctionSourceInfo sourceInfo = GraphReaderUtil::getFunctionSourceInfo(llvmFun);
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
 * \brief 根据函数名查找并打印函数的函数体。
 *
 * 1. 从 ICFG 获取 SVFIR (PAG)。
 * 2. 使用 SVFIR::getFun() 方法通过函数名查找对应的 FunObjVar (SVF的函数表示)。
 * 3. 如果未找到函数，则报告错误。
 * 4. 获取 LLVMModuleSet 单例，用于在 SVF IR 和 LLVM IR 之间进行转换。
 * 5. 使用 LLVMModuleSet::getLLVMValue() 将 FunObjVar 转换为 llvm::Value。
 * 6. 将 llvm::Value 动态转换为 llvm::Function。
 * 7. 调用辅助函数 getFunctionSourceInfo() 从 llvm::Function 中提取源文件信息（文件名、起始和结束行号）。
 * 8. 将获取到的信息格式化为 JSON 对象并打印到标准输出。
 *
 * \param icfg 指向ICFG的指针。
 * \param functionName 要查找的函数名。
 */
void printFunctionBodyByName(ICFG* icfg, const std::string& functionName) {
    llvm::json::Object result;

    SVFIR* pag = SVFIR::getPAG();
    const FunObjVar* svfFun = pag->getFunObjVar(functionName);

    if (!svfFun) {
        result["error"] = true;
        result["message"] = "Function '" + functionName + "' not found.";
        llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
        return;
    }

    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
    const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(svfFun);
    const llvm::Function* llvmFun = SVFUtil::dyn_cast<llvm::Function>(llvmVal);

    if (!llvmFun) {
        result["error"] = true;
        result["message"] = "Could not retrieve LLVM function for '" + functionName + "'.";
        llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
        return;
    }

    FunctionSourceInfo sourceInfo = GraphReaderUtil::getFunctionSourceInfo(llvmFun);

    result["function_name"] = svfFun->getName();
    result["filename"] = sourceInfo.filename;
    result["start_line"] = sourceInfo.startLine;
    result["end_line"] = sourceInfo.endLine;
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
                        std::string formattedLoc = "unknown";

                        llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(locString);
                        if (auto file = locInfo.getString("fl")) {
                            if (auto line = locInfo.getInteger("ln")) {
                                formattedLoc = file->str() + ":" + std::to_string(*line);
                            }
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
 * \brief 根据源代码位置和可选的操作数索引查找SVFGNode。
 *
 * 这是一个更可靠的映射方法，解决了ICFGNode到SVFGNode一对多的问题。
 * 1. 从 location 找到 ICFGNode，然后获取对应的 llvm::Instruction。
 * 2. 收集指令的定义值（LHS）和所有操作数（RHS）。
 * 3. 将它们都转换为 SVFGNode，并存储在一个列表中。
 * 4. 用户可以通过 operandIndex 选择想要的节点：
 *    - -1 (默认): 返回定义值 (LHS) 对应的节点。
 *    - 0, 1, 2...: 返回第 n 个操作数 (RHS) 对应的节点。
 *
 * \param svfg 指向SVFG的指针。
 * \param icfg 指向ICFG的指针。
 * \param location 形如 "filename:line" 的字符串。
 * \param operandIndex 操作数索引。
 * \return 如果找到，则为匹配的SVFGNode指针，否则为nullptr。
 */
const SVFGNode* findSVFGNodeByLocation(SVFG* svfg, ICFG* icfg, const std::string& location, int operandIndex = -1) {
    std::string opIndexStr = (operandIndex == -1) ? "LHS (defined value)" : std::to_string(operandIndex);
    SVF::SVFUtil::outs() << "Debug: Searching for SVFGNode at location '" << location << "' with operand index " << opIndexStr << "...\n";
    const ICFGNode* icfgNode = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
    if (!icfgNode) {
        SVF::SVFUtil::errs() << "Error: Cannot find ICFGNode for location: " << location << "\n";
        return nullptr;
    }

    SVF::SVFUtil::outs() << "Debug: Found ICFGNode with ID: " << icfgNode->getId() << "\n";

    // Get the LLVM instruction directly from the ICFGNode.
    // This is more robust than relying on SVFStmts, as some instructions (like 'free')
    // have an ICFGNode but no SVFStmt. We handle different ICFGNode types.
    const llvm::Instruction* inst = nullptr;
    if (auto intraNode = SVFUtil::dyn_cast<IntraICFGNode>(icfgNode)) {
        const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(intraNode);
        inst = SVFUtil::dyn_cast<llvm::Instruction>(llvmVal);
    } else if (auto callNode = SVFUtil::dyn_cast<CallICFGNode>(icfgNode)) {
        const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(callNode);
        inst = SVFUtil::dyn_cast<llvm::Instruction>(llvmVal);
    } else if (auto retNode = SVFUtil::dyn_cast<RetICFGNode>(icfgNode)) {
        const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(retNode);
        inst = SVFUtil::dyn_cast<llvm::Instruction>(llvmVal);
    }
    SVF::SVFUtil::errs() << "retrieveD LLVM instruction from ICFGNode at " << location << ".\n";
    if (!inst) {
        SVF::SVFUtil::errs() << "Error: Could not retrieve LLVM instruction from ICFGNode at " << location << ".\n";
        if (icfgNode->getSVFStmts().empty()) {
             SVF::SVFUtil::outs() << "Debug: This ICFGNode also has no associated SVF statements.\n";
        }
        return nullptr;
    }

    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

    // Case 1: User wants the LHS (defined value) of the instruction
    if (operandIndex == -1) {
        // Instructions like 'store' have no defined value, but their type is not void.
        // We only consider instructions that actually define a value that can have a pointer.
        if (!inst->getType()->isVoidTy() && llvmModuleSet->hasValueNode(inst)) {
            NodeID varId = llvmModuleSet->getValueNode(inst);
            SVF::SVFUtil::outs() << "Debug: Found LHS SVF Node ID: " << varId << "\n";
            return svfg->getSVFGNode(varId);
        } else {
            SVF::SVFUtil::errs() << "Warning: Instruction at " << location << " does not define a value (e.g., store, free) or has no pointer type. Try specifying an operand index like --start-op 0.\n";
            return nullptr;
        }
    }

    // Case 2: User wants an RHS operand
    if (operandIndex >= 0 && static_cast<unsigned>(operandIndex) < inst->getNumOperands()) {
        const llvm::Value* operand = inst->getOperand(operandIndex);
        if (llvmModuleSet->hasValueNode(operand)) {
            NodeID varId = llvmModuleSet->getValueNode(operand);
            SVF::SVFUtil::outs() << "Debug: Found RHS operand " << operandIndex << " SVF Node ID: " << varId << "\n";
            return svfg->getSVFGNode(varId);
        } else {
            SVF::SVFUtil::errs() << "Error: Operand " << operandIndex << " at " << location << " does not have a corresponding SVF value node.\n";
            return nullptr;
        }
    }

    // Handle invalid index
    SVF::SVFUtil::errs() << "Error: Invalid operand index " << opIndexStr << " for instruction at " << location
                         << ". It has " << inst->getNumOperands() << " operands (0 to " << inst->getNumOperands() - 1 << ").\n";
    return nullptr;
}

std::vector<const SVFVar*> findVarByLocation(SVFIR* pag, const std::string& location) {
    std::vector<const SVFVar*> results;
    for (SVFIR::const_iterator it = pag->begin(), eit = pag->end(); it != eit; ++it) {
        const PAGNode* pagNode = it->second;
        if (SVFUtil::isa<ValVar>(pagNode)) {
            std::string locString = pagNode->getSourceLoc();
            llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(locString);
            if (auto file = locInfo.getString("fl")) {
                if (auto line = locInfo.getInteger("ln")) {
                    std::string formattedLoc = file->str() + ":" + std::to_string(*line);
                    if (formattedLoc == location) {
                        results.push_back(pagNode);
                        SVF::SVFUtil::outs() << "Found ValVar at " << pagNode->getSourceLoc() << "\n";
                    }
                }
            }
        } else if (SVFUtil::isa<ObjVar>(pagNode)) {
            std::string locString = pagNode->getSourceLoc();
            llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(locString);
            if (auto file = locInfo.getString("fl")) {
                if (auto line = locInfo.getInteger("ln")) {
                    std::string formattedLoc = file->str() + ":" + std::to_string(*line);
                    if (formattedLoc == location) {
                        results.push_back(pagNode);
                        SVF::SVFUtil::outs() << "Found ObjVar at " << pagNode->getSourceLoc() << "\n";
                    }
                }
            }
        } else {
            SVF::SVFUtil::outs() << "not a var or obj" << "\n";
        }
    }
    return results;
}


// /*!
//  * \brief 在SVFG上分析从startLocation到targetLocation的路径控制条件。
//  *
//  * 该函数利用了Saber的源-汇分析框架(SrcSnkDDA)来执行此操作。
//  * 1. 查找与源代码位置对应的ICFGNode，然后找到它们在SVFG中的对应节点。
//  * 2. 使用自定义的PathCondQuery分析器（继承自SrcSnkDDA）。
//  * 3. 将startNode设为源(source)，targetNode设为汇(sink)。
//  * 4. 执行前向值流切片，然后是后向相关性切片。
//  * 5. 求解路径条件并以JSON格式输出。
//  *
//  * \param svfg 指向SVFG的指针。
//  * \param icfg 指向ICFG的指针。
//  * \param startLocation 路径分析的起始位置。
//  * \param targetLocation 路径分析的目标位置。
//  */
// void svfgPathCondReader(SVFG* svfg, ICFG* icfg, const std::string& startLocation, const std::string& targetLocation) {
//     llvm::json::Object result;
    
//     // 使用新的、更可靠的查找函数
//     // Options::StartOp() 和 Options::EndOp() 是新添加的命令行选项
//     const SVFGNode* startSVFGNode = findSVFGNodeByLocation(svfg, icfg, startLocation, Options::StartOp());
//     const SVFGNode* targetSVFGNode = findSVFGNodeByLocation(svfg, icfg, targetLocation, Options::EndOp());

//     if (!startSVFGNode || !targetSVFGNode) {
//         result["error"] = true;
//         result["message"] = "Invalid start or target SVFGNode. Please check locations and operand indices.";
//         llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
//         return;
//     }

//     SVF::SVFUtil::outs() << "Starting analysis from SVFGNode " << startSVFGNode->getId() << " to " << targetSVFGNode->getId() << "\n";

//     PathCondQuery query(startSVFGNode, targetSVFGNode);
//     query.analyzeQuery();
//     llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(query.getResult())) << "\n";
// }

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

    // 构建SVFG以供新函数使用
    //AndersenWaveDiff* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    //SaberSVFGBuilder memSSA;
    //SVFG* svfg = memSSA.buildFullSVFG(ander);
    // 分配控制流图的分支条件
    // SaberCondAllocator condAllocator;
    // condAllocator.allocate();

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
    else if (!Options::FindBodyByName().empty()) {
        printFunctionBodyByName(icfg, Options::FindBodyByName());
    }
    else if (!Options::FindVarByLocation().empty()) {
        findVarByLocation(pag, Options::FindVarByLocation());
    }
    else if (!Options::PathCondFuncStart().empty() && !Options::PathCondFuncEnd().empty()) {
        pathCondFuncReader(icfg, Options::PathCondFuncStart(), Options::PathCondFuncEnd());
    }
    // else if (!Options::PathCondStart().empty() && !Options::PathCondEnd().empty()) {
    //     svfgPathCondReader(svfg, icfg, Options::PathCondStart(), Options::PathCondEnd());
    // }
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

//     SVF::SVFUtil::outs() << "Debug: Building SVF Module...\n";
//     LLVMModuleSet::buildSVFModule(moduleNameVec);

//     SVF::SVFUtil::outs() << "Debug: Building SVFIR (PAG)...\n";
//     SVFIRBuilder builder;
//     SVFIR* pag = builder.build();
//     if (!pag) {
//         SVF::SVFUtil::errs() << "Error: Failed to build SVFIR (PAG).\n";
//         return 1;
//     }
//     SVF::SVFUtil::outs() << "Debug: SVFIR (PAG) built successfully.\n";

//     // ICFG* icfg = pag->getICFG();
//     findVarByLocation(pag, "slabs.c:162");
//     findVarByLocation(pag, "items.c:1630");
//     findVarByLocation(pag, "items.c:1625");



//     // SVF::SVFUtil::outs() << "Debug: Building SVFG...\n";
//     // AndersenWaveDiff* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
//     // SaberSVFGBuilder memSSA;
//     // SVFG* svfg = memSSA.buildFullSVFG(ander);
//     // if (!svfg) {
//     //     SVF::SVFUtil::errs() << "Error: Failed to build SVFG.\n";
//     //     return 1;
//     // }
//     // SVF::SVFUtil::outs() << "Debug: SVFG built successfully.\n";

//     // const SVFGNode* node1 = findSVFGNodeByLocation(svfg, icfg, "items.c:1625", 0);
//     // const SVFGNode* node2 = findSVFGNodeByLocation(svfg, icfg, "slabs.c:162", -1);
//     // const SVFGNode* node3 = findSVFGNodeByLocation(svfg, icfg, "items.c:1630", 0);
    
//     // if (node1) {
//     //     SVF::SVFUtil::outs() << "Successfully found SVFGNode. ID: " << node1->getId() << "\n";
//     // } else {
//     //     SVF::SVFUtil::errs() << "Error: findSVFGNodeByLocation returned a null pointer. Cannot get node ID.\n";
//     // }

        
//     // if (node2) {
//     //     SVF::SVFUtil::outs() << "Successfully found SVFGNode. ID: " << node2->getId() << "\n";
//     // } else {
//     //     SVF::SVFUtil::errs() << "Error: findSVFGNodeByLocation returned a null pointer. Cannot get node ID.\n";
//     // }

//     // if (node3) {
//     //     SVF::SVFUtil::outs() << "Successfully found SVFGNode. ID: " << node3->getId() << "\n";
//     // } else {
//     //     SVF::SVFUtil::errs() << "Error: findSVFGNodeByLocation returned a null pointer. Cannot get node ID.\n";
//     // }

// //     Debug: Searching for SVFGNode at location 'items.c:1625' with operand index 0...
// // Debug: Found ICFGNode with ID: 15998
// // retrieveD LLVM instruction from ICFGNode at items.c:1625.
// // Debug: Found RHS operand 0 SVF Node ID: 1726
// // Debug: Searching for SVFGNode at location 'slabs.c:162' with operand index LHS (defined value)...
// // Error: Cannot find ICFGNode for location: slabs.c:162
// // Debug: Searching for SVFGNode at location 'items.c:1630' with operand index 0...
// // Debug: Found ICFGNode with ID: 15686
// // retrieveD LLVM instruction from ICFGNode at items.c:1630.
// // Debug: Found RHS operand 0 SVF Node ID: 21039
// // Successfully found SVFGNode. ID: 1726
// // Error: findSVFGNodeByLocation returned a null pointer. Cannot get node ID.
// // Successfully found SVFGNode. ID: 21039


//     // 示例1：查找两条路径之间的条件
//     //pathCondFuncReader(icfg, "restart.c:76", "restart.c:121");

//     // 示例2：根据代码行号查找并打印其所在函数的函数体
//     // printFunctionBodyByLocation(icfg, "stats_prefix.c:118");

//     // 示例3：根据代码行号查找函数调用，并打印被调用函数的函数体
//     // printCalleeFunctionBodyByLocation(icfg, "stats_prefix.c:118");

//     // 示例4：根据函数名查找其所有被调用的位置
//     // printFunctionCallSites(icfg, "stats_prefix_record_get");

//     return 0;
// }

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
#include "FunctionQuery.h"
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
        GraphReaderUtil::sendJsonError("Invalid start or target location.");
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
    AndersenWaveDiff* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    // SaberSVFGBuilder memSSA;
    // SVFG* svfg = memSSA.buildFullSVFG(ander);
    // 分配控制流图的分支条件
    // SaberCondAllocator condAllocator;
    // condAllocator.allocate();

    ICFG* icfg = ander->getICFG();
    // I've also removed the pathCondReader function as it was redundant with pathCondFuncReader
    // and produced non-JSON output, which is inconsistent with the rest of the tool.
    // If simple text output is needed for debugging, it's better to add a flag
    // to pathCondFuncReader to change its output format.
    if (!Options::PathCondStart().empty() && !Options::PathCondEnd().empty()) {
        // pathCondReader(icfg, Options::PathCondStart(), Options::PathCondEnd());
    }
    FunctionQuery fq(icfg, pag);

    if (!Options::FindCallSites().empty()) {
        fq.findCallSites(Options::FindCallSites());
    }
    else if (!Options::FindCalleeBody().empty()) {
        fq.findCalleeBodyByLocation(Options::FindCalleeBody());
    }
    else if (!Options::FindFuncBody().empty()) {
        fq.findFunctionBodyByLocation(Options::FindFuncBody());
    }
    else if (!Options::FindBodyByName().empty()) {
        fq.findFunctionBodyByName(Options::FindBodyByName());
    }
    else if (!Options::FindVarByLocation().empty()) {
        GraphReaderUtil::findVarByLocation(pag, Options::FindVarByLocation());
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
        fq.findCallSites("stats_prefix_record_get");
    }
    return 0;
}

// graph-reader -find-var-by-location slabs:162 PUT/memcached.bc
// graph-reader -find-var-by-location slabs:163 PUT/memcached.bc
// graph-reader -find-var-by-location slabs:164 PUT/memcached.bc
// graph-reader -find-var-by-location items.c:1557 PUT/memcached.bc
// graph-reader -path-cond-func-start items.c:1557 -path-cond-func-end items.c:1630 PUT/memcached.bc
// graph-reader -find-function-body items.c:1557 PUT/memcached.bc

// graph-reader -find-body-by-name TIFFWriteDirectoryTagCheckedLongArray PUT/libtiff-57449991.bc
// graph-reader -find-function-body tif_dirwrite.c:1893 PUT/libtiff-57449991.bc
// graph-reader -find-var-by-location tif_dirread.c:231 PUT/libtiff-57449991.bc 236
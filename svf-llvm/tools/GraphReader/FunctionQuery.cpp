#include "FunctionQuery.h"
#include "GraphReaderUtil.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVFIR/SVFValue.h"
#include "SVF-LLVM/LLVMModule.h"
#include "Graphs/SVFG.h"
#include "Graphs/SVFGNode.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace SVF;
using namespace SVFUtil;

SVF::FunctionQuery::FunctionQuery(ICFG* i, SVFIR* p, SVFG* s) : icfg(i), pag(p), svfg(s) {}

void SVF::FunctionQuery::findCallSites(const std::string& functionName) {
    llvm::json::Object result;
    llvm::json::Array callSites;
    Set<const ICFGNode*> reportedCallSites;

    for (ICFG::const_iterator it = icfg->begin(), eit = icfg->end(); it != eit; ++it) {
        const ICFGNode* node = it->second;
        if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node)) {
            if (reportedCallSites.count(callNode)) {
                continue;
            }
            for (ICFGEdge* edge : callNode->getOutEdges()) {
                if (SVFUtil::isa<CallCFGEdge>(edge)) {
                    const ICFGNode* calleeEntryNode = edge->getDstNode();
                    const FunObjVar* svfFun = calleeEntryNode->getFun();
                    if (svfFun && svfFun->getName() == functionName) {
                        llvm::json::Object site;
                        llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(callNode->getSourceLoc());
                        if (auto file = locInfo.getString("fl")) {
                            site["fl"] = file->str();
                            site["filename"] = file->str();
                        }
                        if (auto line = locInfo.getInteger("ln")) {
                            site["ln"] = *line;
                            site["line"] = *line;
                        }
                        if (auto col = locInfo.getInteger("cl")) {
                            site["cl"] = *col;
                            site["column"] = *col;
                        }
                        callSites.push_back(std::move(site));
                        reportedCallSites.insert(callNode);
                        break;
                    }
                }
            }
        }
    }
    result["call_sites"] = std::move(callSites);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}

void SVF::FunctionQuery::findCalleeBodyByLocation(const GraphReaderUtil::SourceLocation& location) {
    llvm::json::Object result;
    llvm::json::Array calleeFunctions;

    const ICFGNode* node = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
    if (!node) {
        std::string diagnostic = GraphReaderUtil::getLastLocationLookupDiagnostic();
        GraphReaderUtil::sendJsonError(diagnostic.empty() ? "Could not find ICFGNode for the given location."
                                                          : diagnostic);
        return;
    }

    const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node);
    if (!callNode) {
        GraphReaderUtil::sendJsonError("Node at the given location is not a function call site.");
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
                calleeFunctions.push_back(GraphReaderUtil::getFunctionInfoJson(llvmFun));
            }
        }
    }
    result["callee_functions"] = std::move(calleeFunctions);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}

void SVF::FunctionQuery::findFunctionBodyByLocation(const GraphReaderUtil::SourceLocation& location) {
    llvm::json::Object result;
    const ICFGNode* node = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
    if (!node) {
        std::string diagnostic = GraphReaderUtil::getLastLocationLookupDiagnostic();
        GraphReaderUtil::sendJsonError(diagnostic.empty() ? "Could not find ICFGNode for the given location."
                                                          : diagnostic);
        return;
    }

    const FunObjVar* svfFun = node->getFun();
    if (!svfFun) {
        GraphReaderUtil::sendJsonError("The ICFGNode at the given location is not inside a function.");
        return;
    }

    const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(svfFun);
    const llvm::Function* llvmFun = SVFUtil::dyn_cast<llvm::Function>(llvmVal);

    result = GraphReaderUtil::getFunctionInfoJson(llvmFun);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}

void SVF::FunctionQuery::findFunctionBodyByName(const std::string& functionName) {
    llvm::json::Object result;
    const FunObjVar* svfFun = pag->getFunObjVar(functionName);

    if (!svfFun) {
        GraphReaderUtil::sendJsonError("Function '" + functionName + "' not found.");
        // // 调试输出：当找不到函数时，打印 PAG 中所有可用的函数名
        // SVF::SVFUtil::errs() << "Debug: Available functions in PAG:\n";
        // // 遍历 funArgsListMap 来获取所有的 FunObjVar，然后打印它们的名称
        // for (auto const& [fun, args] : pag->getFunArgsMap()) {
        //     SVF::SVFUtil::errs() << "  - " << fun->getName() << "\n";
        // }
        return;
    }

    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
    const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(svfFun);
    const llvm::Function* llvmFun = SVFUtil::dyn_cast<llvm::Function>(llvmVal);

    if (!llvmFun) {
        GraphReaderUtil::sendJsonError("Could not retrieve LLVM function for '" + functionName + "'.");
        return;
    }

    result = GraphReaderUtil::getFunctionInfoJson(llvmFun);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}

void SVF::FunctionQuery::findAllCalleesByName(const std::string& functionName) {
    llvm::json::Object result;
    const CallGraph* callGraph = pag->getCallGraph();
    const FunObjVar* startFun = pag->getFunObjVar(functionName);

    if (!startFun) {
        GraphReaderUtil::sendJsonError("Function '" + functionName + "' not found.");
        return;
    }

    const CallGraphNode* startNode = callGraph->getCallGraphNode(startFun);
    if (!startNode) {
        GraphReaderUtil::sendJsonError("Could not find CallGraphNode for function '" + functionName + "'.");
        return;
    }

    llvm::json::Array calleesArray;
    Set<std::string> calleeNames;
    std::queue<const CallGraphNode*> worklist;
    Set<const CallGraphNode*> visited;

    worklist.push(startNode);
    visited.insert(startNode);

    while (!worklist.empty()) {
        const CallGraphNode* currentNode = worklist.front();
        worklist.pop();

        for (const CallGraphEdge* edge : currentNode->getOutEdges()) {
            const CallGraphNode* calleeNode = edge->getDstNode();
            const FunObjVar* calleeFun = calleeNode->getFunction();
            if (calleeFun && calleeNames.find(calleeFun->getName()) == calleeNames.end()) {
                calleeNames.insert(calleeFun->getName());
                calleesArray.push_back(calleeFun->getName());
            }

            if (visited.find(calleeNode) == visited.end()) {
                visited.insert(calleeNode);
                worklist.push(calleeNode);
            }
        }
    }

    result["function"] = functionName;
    result["callees"] = std::move(calleesArray);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}

void SVF::FunctionQuery::checkFunctionAlwaysReturn(const std::string& functionName) {
    const FunObjVar* targetFunction = pag->getFunObjVar(functionName);
    if (!targetFunction) {
        GraphReaderUtil::sendJsonError("Function '" + functionName + "' not found.");
        return;
    }

    FunEntryICFGNode* entryNode = icfg->getFunEntryICFGNode(targetFunction);
    FunExitICFGNode* exitNode = icfg->getFunExitICFGNode(targetFunction);

    if (!entryNode || !exitNode) {
        GraphReaderUtil::sendJsonError("Could not locate entry/exit nodes for function '" + functionName + "'.");
        return;
    }

    std::vector<const CallICFGNode*> callNodes;
    bool hasFunctionNodes = false;

    auto isInFunction = [&](const ICFGNode* node) -> bool {
        return node && node->getFun() == targetFunction;
    };

    for (ICFG::const_iterator it = icfg->begin(), eit = icfg->end(); it != eit; ++it) {
        const ICFGNode* node = it->second;
        if (!isInFunction(node)) {
            continue;
        }
        hasFunctionNodes = true;
        if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node)) {
            callNodes.push_back(callNode);
        }
    }

    if (!hasFunctionNodes) {
        GraphReaderUtil::sendJsonError("Function '" + functionName + "' has no associated ICFG nodes.");
        return;
    }

    auto formatLocation = [](const ICFGNode* node) -> std::string {
        if (!node) {
            return std::string("unknown");
        }
        std::string rawLoc = node->getSourceLoc();
        if (rawLoc.empty()) {
            return std::string("unknown");
        }
        std::string formatted = "unknown";
        llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(rawLoc);
        if (auto file = locInfo.getString("fl")) {
            formatted = file->str();
            if (auto line = locInfo.getInteger("ln")) {
                formatted += ":" + std::to_string(*line);
            }
        }
        return formatted;
    };

    auto emitResult = [&](bool alwaysReturn, const std::string& location) {
        llvm::json::Object result;
        result["always_return"] = alwaysReturn ? "true" : "false";
        if (alwaysReturn) {
            result["error_loction"] = nullptr;
        } else {
            result["error_loction"] = location;
        }
        result["error"] = false;
        llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
        llvm::outs().flush();
    };

    auto isTerminationCallName = [](llvm::StringRef name) -> bool {
        static const char* exactMatches[] = {
            "exit", "_exit", "_Exit", "abort", "quick_exit", "std::terminate"
        };
        for (const char* exact : exactMatches) {
            if (name.equals(exact)) {
                return true;
            }
        }
        if (name.startswith("exec") || name.startswith("_exec")) {
            return true;
        }
        return false;
    };

    // Strategy 1: Detect explicit termination calls (exit/exec/abort).
    for (const CallICFGNode* callNode : callNodes) {
        const FunObjVar* callee = callNode->getCalledFunction();
        if (!callee) {
            continue;
        }
        llvm::StringRef calleeName = callee->getName();
        if (isTerminationCallName(calleeName)) {
            emitResult(false, formatLocation(callNode) + " (terminating call: " + calleeName.str() + ")");
            return;
        }
    }

    // Strategy 2: Ensure all reachable paths from entry can reach the function exit.
    Set<const ICFGNode*> reachable;
    std::queue<const ICFGNode*> worklist;
    worklist.push(entryNode);
    reachable.insert(entryNode);

    while (!worklist.empty()) {
        const ICFGNode* current = worklist.front();
        worklist.pop();
        for (ICFGEdge* edge : current->getOutEdges()) {
            const ICFGNode* succ = edge->getDstNode();
            if (!isInFunction(succ)) {
                continue;
            }
            if (reachable.insert(succ).second) {
                worklist.push(succ);
            }
        }
    }

    if (reachable.find(exitNode) == reachable.end()) {
        emitResult(false, formatLocation(entryNode) + " (function exit unreachable)");
        return;
    }

    Set<const ICFGNode*> canReachExit;
    std::queue<const ICFGNode*> backward;
    backward.push(exitNode);
    canReachExit.insert(exitNode);

    while (!backward.empty()) {
        const ICFGNode* current = backward.front();
        backward.pop();
        for (ICFGEdge* edge : current->getInEdges()) {
            const ICFGNode* pred = edge->getSrcNode();
            if (!isInFunction(pred)) {
                continue;
            }
            if (canReachExit.insert(pred).second) {
                backward.push(pred);
            }
        }
    }

    for (const ICFGNode* node : reachable) {
        if (canReachExit.find(node) != canReachExit.end()) {
            continue;
        }
        bool inLoop = icfg->isInLoop(node);
        std::string location = formatLocation(node);
        if (inLoop) {
            location += " (possible infinite loop)";
        } else {
            location += " (no path to return)";
        }
        emitResult(false, location);
        return;
    }

    emitResult(true, "");
}

void SVF::FunctionQuery::checkReturnPointer(const GraphReaderUtil::SourceLocation& location) {
    llvm::json::Object result;
    
    // Find all ICFG nodes at this location
    std::vector<const ICFGNode*> nodes = GraphReaderUtil::findAllICFGNodesByLocation(icfg, location);
    
    if (nodes.empty()) {
        std::string diagnostic = GraphReaderUtil::getLastLocationLookupDiagnostic();
        GraphReaderUtil::sendJsonError(diagnostic.empty()
                                           ? "Could not find any ICFGNode at the given location: " + GraphReaderUtil::toString(location)
                                           : diagnostic);
        return;
    }
    
    // Get the function from any of the nodes at this location
    const FunObjVar* targetFunction = nullptr;
    for (const ICFGNode* node : nodes) {
        targetFunction = node->getFun();
        if (targetFunction) {
            break;
        }
    }
    
    if (!targetFunction) {
        GraphReaderUtil::sendJsonError("Could not determine the function at location: " + GraphReaderUtil::toString(location));
        return;
    }
    
    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
    
    // ===== Strategy 1: Check if function can return pointer type =====
    // Find the isRetInst node to check if function's return type is pointer
    bool functionCanReturnPointer = false;
    for (auto it = icfg->begin(); it != icfg->end(); ++it) {
        const ICFGNode* node = it->second;
        if (!node || node->getFun() != targetFunction) {
            continue;
        }
        
        const IntraICFGNode* intraNode = SVFUtil::dyn_cast<IntraICFGNode>(node);
        if (!intraNode || !intraNode->isRetInst()) {
            continue;
        }
        
        const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(intraNode);
        const llvm::Instruction* inst = llvm::dyn_cast<llvm::Instruction>(llvmVal);
        if (!inst) {
            continue;
        }
        
        const llvm::ReturnInst* retInst = llvm::dyn_cast<llvm::ReturnInst>(inst);
        if (retInst) {
            llvm::Value* retValue = retInst->getReturnValue();
            if (retValue && retValue->getType()->isPointerTy()) {
                functionCanReturnPointer = true;
                break;
            }
        }
    }
    
    // ===== Strategy 2: Check if this specific location has pointer-producing operations =====
    // Look for LoadVFGNode, GepVFGNode, AddrVFGNode, CopyVFGNode with pointer type
    bool locationHasPointerOperation = false;
    llvm::json::Array svfgNodeInfoArray;
    
    if (functionCanReturnPointer && svfg) {
        // Only check location if function can return pointer
        for (const ICFGNode* node : nodes) {
            // Get SVFG nodes associated with this ICFG node using VFGNodes list
            for (const VFGNode* vfgNode : node->getVFGNodes()) {
                const SVFGNode* svfgNode = SVFUtil::dyn_cast<SVFGNode>(vfgNode);
                if (!svfgNode) continue;
                
                bool isPointerProducing = false;
                std::string nodeTypeStr = "Unknown";
                
                // Check for pointer-producing SVFG nodes
                if (const LoadVFGNode* loadNode = SVFUtil::dyn_cast<LoadVFGNode>(svfgNode)) {
                    if (loadNode->getPAGDstNode()->isPointer()) {
                        isPointerProducing = true;
                        nodeTypeStr = "LoadVFGNode";
                    }
                } else if (SVFUtil::isa<AddrVFGNode>(svfgNode)) {
                    isPointerProducing = true;
                    nodeTypeStr = "AddrVFGNode";
                } else if (SVFUtil::isa<GepVFGNode>(svfgNode)) {
                    isPointerProducing = true;
                    nodeTypeStr = "GepVFGNode";
                } else if (const CopyVFGNode* copyNode = SVFUtil::dyn_cast<CopyVFGNode>(svfgNode)) {
                    if (copyNode->getPAGDstNode()->isPointer()) {
                        isPointerProducing = true;
                        nodeTypeStr = "CopyVFGNode";
                    }
                }
                
                if (isPointerProducing) {
                    locationHasPointerOperation = true;
                    
                    llvm::json::Object svfgInfo;
                    svfgInfo["svfg_node_id"] = static_cast<int64_t>(svfgNode->getId());
                    svfgInfo["svfg_node_type"] = nodeTypeStr;
                    svfgInfo["icfg_node_id"] = static_cast<int64_t>(node->getId());
                    svfgNodeInfoArray.push_back(std::move(svfgInfo));
                }
            }
        }
    } else if (!svfg) {
        // If no SVFG is available, we can only use strategy 1
        // In this case, we conservatively return true if function can return pointer
        locationHasPointerOperation = functionCanReturnPointer;
    }
    
    // ===== Final Decision: Both conditions must be true =====
    bool returnsPointer = functionCanReturnPointer && locationHasPointerOperation;
    
    // Prepare result
    result = GraphReaderUtil::toJson(location);
    result["function_name"] = targetFunction->getName();
    result["function_can_return_pointer"] = functionCanReturnPointer;
    result["location_has_pointer_operation"] = locationHasPointerOperation;
    result["returnptr"] = returnsPointer;
    
    if (returnsPointer && !svfgNodeInfoArray.empty()) {
        result["pointer_producing_nodes"] = std::move(svfgNodeInfoArray);
    } else {
        result["pointer_producing_nodes"] = nullptr;
    }
    
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}

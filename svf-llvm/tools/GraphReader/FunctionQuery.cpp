#include "FunctionQuery.h"
#include "GraphReaderUtil.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVFIR/SVFValue.h"
#include "SVF-LLVM/LLVMModule.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>

using namespace llvm;
using namespace SVF;
using namespace SVFUtil;

SVF::FunctionQuery::FunctionQuery(ICFG* i, SVFIR* p) : icfg(i), pag(p) {}

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

void SVF::FunctionQuery::findCalleeBodyByLocation(const std::string& location) {
    llvm::json::Object result;
    llvm::json::Array calleeFunctions;

    const ICFGNode* node = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
    if (!node) {
        GraphReaderUtil::sendJsonError("Could not find ICFGNode for the given location.");
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

void SVF::FunctionQuery::findFunctionBodyByLocation(const std::string& location) {
    llvm::json::Object result;
    const ICFGNode* node = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
    if (!node) {
        GraphReaderUtil::sendJsonError("Could not find ICFGNode for the given location.");
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

void SVF::FunctionQuery::findRetLocations(const std::string& functionName, const std::string& location) {
    llvm::json::Object result;
    
    // Find the function
    const FunObjVar* targetFun = pag->getFunObjVar(functionName);
    if (!targetFun) {
        GraphReaderUtil::sendJsonError("Function '" + functionName + "' not found.");
        return;
    }
    
    // Step 1: Find all exit nodes (isRetInst=1 nodes or function exit nodes)
    // and nodes that directly point to them
    Set<NodeID> exitNodeIDs;
    Set<NodeID> coreExitNodes;
    
    // Step 1.1: First, find all isRetInst=1 and FunExitICFGNode nodes
    for (ICFG::const_iterator it = icfg->begin(), eit = icfg->end(); it != eit; ++it) {
        const ICFGNode* node = it->second;
        const FunObjVar* nodeFun = node->getFun();
        if (nodeFun && nodeFun->getName() == functionName) {
            // Check if this is an IntraICFGNode with isRetInst=1
            if (const IntraICFGNode* intraNode = SVFUtil::dyn_cast<IntraICFGNode>(node)) {
                if (intraNode->isRetInst()) {
                    coreExitNodes.insert(node->getId());
                    exitNodeIDs.insert(node->getId());
                }
            }
            // Also check if this is a FunExitICFGNode
            if (SVFUtil::isa<FunExitICFGNode>(node)) {
                coreExitNodes.insert(node->getId());
                exitNodeIDs.insert(node->getId());
            }
        }
    }
    
    // Step 1.2: Find all nodes that directly point to the core exit nodes
    for (ICFG::const_iterator it = icfg->begin(), eit = icfg->end(); it != eit; ++it) {
        const ICFGNode* node = it->second;
        const FunObjVar* nodeFun = node->getFun();
        if (nodeFun && nodeFun->getName() == functionName) {
            for (const ICFGEdge* edge : node->getOutEdges()) {
                if (coreExitNodes.count(edge->getDstID())) {
                    exitNodeIDs.insert(node->getId());
                }
            }
        }
    }
    
    // Find the starting node by location
    const ICFGNode* startNode = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
    if (!startNode) {
        GraphReaderUtil::sendJsonError("Could not find ICFGNode for the given location.");
        return;
    }
    
    // Verify the node is in the target function
    const FunObjVar* nodeFun = startNode->getFun();
    if (!nodeFun) {
        GraphReaderUtil::sendJsonError("The given location is not inside any function.");
        return;
    }
    
    if (nodeFun->getName() != functionName) {
        GraphReaderUtil::sendJsonError("The given location is in function '" + nodeFun->getName() + 
                                       "', not in '" + functionName + "'.");
        return;
    }
    
    // BFS traversal to find all reachable return nodes
    llvm::json::Array returnLocations;
    Set<const ICFGNode*> visited;
    std::queue<const ICFGNode*> worklist;
    Set<std::string> reportedLocations; // To avoid duplicate locations
    
    worklist.push(startNode);
    visited.insert(startNode);
    
    while (!worklist.empty()) {
        const ICFGNode* currentNode = worklist.front();
        worklist.pop();
        
        // Check if this node points to any exit node
        // But exclude nodes that themselves point to core exit nodes (they are intermediate nodes)
        bool pointsToExitNode = false;
        bool pointsToCoreExitNode = false;
        
        for (const ICFGEdge* edge : currentNode->getOutEdges()) {
            const ICFGNode* dstNode = edge->getDstNode();
            NodeID dstID = dstNode->getId();
            
            if (coreExitNodes.count(dstID)) {
                // This node directly points to a core exit node (isRetInst=1)
                // It's an intermediate node (like 60829), not a real return statement
                pointsToCoreExitNode = true;
                break;
            }
            
            if (exitNodeIDs.count(dstID)) {
                pointsToExitNode = true;
            }
        }
        
        // Only report if it points to exit nodes but NOT directly to core exit nodes
        if (pointsToExitNode && !pointsToCoreExitNode) {
            std::string locString = currentNode->getSourceLoc();
            std::string formattedLoc = "unknown";
            
            llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(locString);
            if (auto file = locInfo.getString("fl")) {
                if (auto line = locInfo.getInteger("ln")) {
                    formattedLoc = file->str() + ":" + std::to_string(*line);
                }
            }
            
            // Avoid duplicate locations (same line might have multiple nodes)
            if (reportedLocations.find(formattedLoc) == reportedLocations.end()) {
                reportedLocations.insert(formattedLoc);
                
                llvm::json::Object retLocation;
                retLocation["location"] = formattedLoc;
                returnLocations.push_back(std::move(retLocation));
            }
        }
        
        // Special handling for CallICFGNode: jump directly to its return node
        if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(currentNode)) {
            const RetICFGNode* retNode = callNode->getRetICFGNode();
            if (retNode) {
                const FunObjVar* retFun = retNode->getFun();
                if (retFun && retFun->getName() == functionName) {
                    if (visited.find(retNode) == visited.end()) {
                        visited.insert(retNode);
                        worklist.push(retNode);
                    }
                }
            }
            continue;
        }
        
        // Traverse outgoing edges for non-call nodes
        for (ICFGEdge* edge : currentNode->getOutEdges()) {
            const ICFGNode* nextNode = edge->getDstNode();
            
            // Skip call edges (edges from call site to callee entry)
            if (SVFUtil::isa<CallCFGEdge>(edge)) {
                continue;
            }
            
            // For return edges (edges from callee exit back to caller),
            // check if the destination is in our target function
            if (SVFUtil::isa<RetCFGEdge>(edge)) {
                const FunObjVar* nextFun = nextNode->getFun();
                if (nextFun && nextFun->getName() == functionName) {
                    if (visited.find(nextNode) == visited.end()) {
                        visited.insert(nextNode);
                        worklist.push(nextNode);
                    }
                }
                continue;
            }
            
            // For other edges, only continue if the next node is in the same function
            const FunObjVar* nextFun = nextNode->getFun();
            if (!nextFun || nextFun->getName() != functionName) {
                continue;
            }
            
            if (visited.find(nextNode) == visited.end()) {
                visited.insert(nextNode);
                worklist.push(nextNode);
            }
        }
    }
    
    result["function"] = functionName;
    result["start_location"] = location;
    result["return_locations"] = std::move(returnLocations);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}
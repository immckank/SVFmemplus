#include "FunctionQuery.h"
#include "GraphReaderUtil.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVFIR/SVFValue.h"
#include "SVF-LLVM/LLVMModule.h"
#include "Graphs/SVFG.h"
#include "Graphs/SVFGNode.h"
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

void SVF::FunctionQuery::checkReturnPointer(const std::string& location) {
    llvm::json::Object result;
    
    // Find all ICFG nodes at this location
    std::vector<const ICFGNode*> nodes = GraphReaderUtil::findAllICFGNodesByLocation(icfg, location);
    
    if (nodes.empty()) {
        GraphReaderUtil::sendJsonError("Could not find any ICFGNode at the given location: " + location);
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
        GraphReaderUtil::sendJsonError("Could not determine the function at location: " + location);
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
    result["location"] = location;
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

void SVF::FunctionQuery::showFunctionReturnInfo(const std::string& location) {
    // Find all ICFG nodes at this location
    std::vector<const ICFGNode*> nodes = GraphReaderUtil::findAllICFGNodesByLocation(icfg, location);
    
    if (nodes.empty()) {
        GraphReaderUtil::sendJsonError("Could not find any ICFGNode at the given location: " + location);
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
        GraphReaderUtil::sendJsonError("Could not determine the function at location: " + location);
        return;
    }
    
    SVF::SVFUtil::outs() << "\n========================================\n";
    SVF::SVFUtil::outs() << "Function Return Info for: " << targetFunction->getName() << "\n";
    SVF::SVFUtil::outs() << "Query Location: " << location << "\n";
    SVF::SVFUtil::outs() << "========================================\n\n";
    
    // Find all return instructions in this function
    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
    std::vector<const IntraICFGNode*> retNodes;
    
    for (auto it = icfg->begin(); it != icfg->end(); ++it) {
        const ICFGNode* node = it->second;
        if (!node || node->getFun() != targetFunction) {
            continue;
        }
        
        const IntraICFGNode* intraNode = SVFUtil::dyn_cast<IntraICFGNode>(node);
        if (intraNode && intraNode->isRetInst()) {
            retNodes.push_back(intraNode);
        }
    }
    
    SVF::SVFUtil::outs() << "Total isRetInst nodes in function: " << retNodes.size() << "\n\n";
    
    if (retNodes.empty()) {
        SVF::SVFUtil::outs() << "No return instruction nodes found in this function.\n";
        SVF::SVFUtil::outs() << "========================================\n\n";
        return;
    }
    
    // Display details for each return node
    for (size_t i = 0; i < retNodes.size(); i++) {
        const IntraICFGNode* retNode = retNodes[i];
        
        SVF::SVFUtil::outs() << "----------------------------------------\n";
        SVF::SVFUtil::outs() << "Return Node #" << (i + 1) << ":\n";
        SVF::SVFUtil::outs() << "----------------------------------------\n";
        SVF::SVFUtil::outs() << "  ICFG Node ID: " << retNode->getId() << "\n";
        
        // Get source location
        llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(retNode->getSourceLoc());
        if (auto file = locInfo.getString("fl")) {
            SVF::SVFUtil::outs() << "  File: " << file->str() << "\n";
        }
        if (auto line = locInfo.getInteger("ln")) {
            SVF::SVFUtil::outs() << "  Line: " << *line << "\n";
        }
        if (auto col = locInfo.getInteger("cl")) {
            SVF::SVFUtil::outs() << "  Column: " << *col << "\n";
        }
        
        // Get the LLVM instruction
        const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(retNode);
        const llvm::Instruction* inst = llvm::dyn_cast<llvm::Instruction>(llvmVal);
        
        if (!inst) {
            SVF::SVFUtil::outs() << "  LLVM Instruction: Not available\n";
            continue;
        }
        
        SVF::SVFUtil::outs() << "  LLVM Instruction Type: " << inst->getOpcodeName() << "\n";
        
        // Check if this is a ReturnInst
        const llvm::ReturnInst* retInst = llvm::dyn_cast<llvm::ReturnInst>(inst);
        if (retInst) {
            SVF::SVFUtil::outs() << "  Is ReturnInst: true\n";
            
            llvm::Value* retValue = retInst->getReturnValue();
            if (retValue) {
                // Get return value type
                std::string retTypeStr;
                llvm::raw_string_ostream rso(retTypeStr);
                retValue->getType()->print(rso);
                SVF::SVFUtil::outs() << "  Return Value Type: " << rso.str() << "\n";
                SVF::SVFUtil::outs() << "  Is Pointer Type: " << (retValue->getType()->isPointerTy() ? "true" : "false") << "\n";
                
                // Show the return value itself
                std::string retValStr;
                llvm::raw_string_ostream rvso(retValStr);
                retValue->print(rvso);
                SVF::SVFUtil::outs() << "  Return Value: " << rvso.str() << "\n";
                
                // Check if it's a PHI node
                if (llvm::isa<llvm::PHINode>(retValue)) {
                    SVF::SVFUtil::outs() << "  Return Value is PHI Node: true\n";
                    const llvm::PHINode* phiNode = llvm::cast<llvm::PHINode>(retValue);
                    SVF::SVFUtil::outs() << "  PHI Node has " << phiNode->getNumIncomingValues() << " incoming values:\n";
                    for (unsigned j = 0; j < phiNode->getNumIncomingValues(); j++) {
                        llvm::Value* incomingVal = phiNode->getIncomingValue(j);
                        std::string incomingStr;
                        llvm::raw_string_ostream ivso(incomingStr);
                        incomingVal->print(ivso);
                        SVF::SVFUtil::outs() << "    Incoming #" << j << ": " << ivso.str() << "\n";
                    }
                } else {
                    SVF::SVFUtil::outs() << "  Return Value is PHI Node: false\n";
                }
            } else {
                SVF::SVFUtil::outs() << "  Return Value: void\n";
            }
        } else {
            SVF::SVFUtil::outs() << "  Is ReturnInst: false (unexpected!)\n";
        }
        
        SVF::SVFUtil::outs() << "\n";
    }
    
    SVF::SVFUtil::outs() << "========================================\n";
    SVF::SVFUtil::outs() << "End of Function Return Info\n";
    SVF::SVFUtil::outs() << "========================================\n\n";
}
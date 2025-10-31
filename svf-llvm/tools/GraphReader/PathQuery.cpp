#include "PathQuery.h"
#include "GraphReaderUtil.h"
#include "Graphs/ICFG.h"
#include "Graphs/SVFGEdge.h"
#include "SVFIR/SVFValue.h"
#include "Graphs/VFGNode.h"
#include "Graphs/SVFG.h"
#include "Util/SVFUtil.h"
#include "SVF-LLVM/LLVMModule.h"
#include <llvm/IR/Instructions.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <iostream>
#include <map>
#include <queue>
#include <set>

namespace SVF {

// Helper function to get SVFGNode kind as string
static std::string getSVFGNodeKindString(const SVFGNode* node) {
    if (!node) return "Null";
    switch (node->getNodeKind()) {
        case SVFValue::Addr: return "Addr";
        case SVFValue::Copy: return "Copy";
        case SVFValue::Gep: return "Gep";
        case SVFValue::Store: return "Store";
        case SVFValue::Load: return "Load";
        case SVFValue::Cmp: return "Cmp";
        case SVFValue::BinaryOp: return "BinaryOp";
        case SVFValue::UnaryOp: return "UnaryOp";
        case SVFValue::Branch: return "Branch";
        case SVFValue::DummyVProp: return "DummyVProp";
        case SVFValue::NPtr: return "NPtr";
        case SVFValue::FRet: return "FRet";
        case SVFValue::ARet: return "ARet";
        case SVFValue::AParm: return "AParm";
        case SVFValue::FParm: return "FParm";
        case SVFValue::TPhi: return "TPhi";
        case SVFValue::TIntraPhi: return "TIntraPhi";
        case SVFValue::TInterPhi: return "TInterPhi";
        case SVFValue::FPIN: return "FPIN";
        case SVFValue::FPOUT: return "FPOUT";
        case SVFValue::APIN: return "APIN";
        case SVFValue::APOUT: return "APOUT";
        case SVFValue::MPhi: return "MPhi";
        case SVFValue::MIntraPhi: return "MIntraPhi";
        case SVFValue::MInterPhi: return "MInterPhi";
        default: return "Unknown";
    }
}

void PathQuery::getValuePath(const SVFGNode* startNode) {
    if (!startNode) {
        GraphReaderUtil::sendJsonError("Error: Start node is null.");
        return;
    }

    SVFUtil::outs() << "Starting value-flow path analysis from SVFGNode " << startNode->getId()
                    << " (" << startNode->toString() << ")\n";
    SVFUtil::outs() << "Location: " << startNode->getSourceLoc() << "\n";
    SVFUtil::outs() << "--------------------------------------------------\n";

    SVFGPath initialPath;
    std::set<const SVFGNode*> visited;

    // Start DFS from the initial node
    dfsVisit(startNode, initialPath, visited);
}

void PathQuery::getValueInsidePath(const SVFGNode* startNode) {
    // Records all locations where the variable corresponding to the currently concerned node
    // is used or modified within the current function.

    if (!startNode) {
        GraphReaderUtil::sendJsonError("Error: Start node is null for getValueInsidePath.");
        return;
    }

    const FunObjVar* startFunction = startNode->getFun();
    if (!startFunction) {
        GraphReaderUtil::sendJsonError("Error: Start node does not belong to a function.");
        return;
    }

    SVFUtil::errs() << "Starting intra-procedural value-flow analysis from SVFGNode " << startNode->getId()
                    << " (" << startNode->toString() << ")\n";
    SVFUtil::errs() << "Function: " << startFunction->getName() << "\n";
    SVFUtil::errs() << "--------------------------------------------------\n";

    std::set<const SVFGNode*> visited;
    std::queue<const SVFGNode*> worklist;

    // Start BFS from the initial node
    worklist.push(startNode);
    visited.insert(startNode);

    // Map: location key -> {location_obj, list of {node_id, node_kind}}
    // Location key format: "file:line:column" or empty if no location
    std::map<std::string, std::pair<llvm::json::Object, std::vector<std::pair<NodeID, std::string>>>> locationMap;

    auto processNode = [&](const SVFGNode* node) {
        llvm::json::Object locObj;
        // if (const ActualParmVFGNode* apNode = SVFUtil::dyn_cast<ActualParmVFGNode>(node)) {
        //     // 忽略这个 这个是参数传递 不是使用
        //     (void)apNode;
        //     // const CallICFGNode* callSite = apNode->getCallSite();
        //     // if (callSite) {
        //     //     std::string callSiteLoc = callSite->getSourceLoc();
        //     //     if (!callSiteLoc.empty()) {
        //     //         locObj = GraphReaderUtil::parseSourceLocation(callSiteLoc);
        //     //     }
        //     // }
        // } else if (const ActualRetVFGNode* arNode = SVFUtil::dyn_cast<ActualRetVFGNode>(node)) {
        //     (void)arNode;
        //     // const CallICFGNode* callSite = arNode->getCallSite();
        //     // if (callSite) {
        //     //     std::string callSiteLoc = callSite->getSourceLoc();
        //     //     if (!callSiteLoc.empty()) {
        //     //         locObj = GraphReaderUtil::parseSourceLocation(callSiteLoc);
        //     //     }
        //     // }
        // } else if (const ActualINSVFGNode* aiNode = SVFUtil::dyn_cast<ActualINSVFGNode>(node)) {
        //     const CallICFGNode* callSite = aiNode->getCallSite();
        //     if (callSite) {
        //         std::string callSiteLoc = callSite->getSourceLoc();
        //         if (!callSiteLoc.empty()) {
        //             locObj = GraphReaderUtil::parseSourceLocation(callSiteLoc);
        //         }
        //     }
        // } else if (const ActualOUTSVFGNode* aoNode = SVFUtil::dyn_cast<ActualOUTSVFGNode>(node)) {
        //     const CallICFGNode* callSite = aoNode->getCallSite();
        //     if (callSite) {
        //         std::string callSiteLoc = callSite->getSourceLoc();
        //         if (!callSiteLoc.empty()) {
        //             locObj = GraphReaderUtil::parseSourceLocation(callSiteLoc);
        //         }
        //     }
        // }
        // 优化后的parseSourceLocation几乎可以找到所有类型的节点的位置信息
        if (locObj.empty()) {
            std::string nodeStr = node->toString();
            locObj = GraphReaderUtil::parseSourceLocation(nodeStr);
        }
        
        // Create location key
        std::string locKey;
        if (auto file = locObj.getString("fl")) {
            if (auto line = locObj.getInteger("ln")) {
                locKey = file->str() + ":" + std::to_string(*line);
                if (auto col = locObj.getInteger("cl")) {
                    locKey += ":" + std::to_string(*col);
                }
            }
        }
        
        // Skip nodes without valid location information
        if (locKey.empty()) {
            SVFUtil::errs() << "  [-] Skipping Node " << node->getId()
                            << " [" << getSVFGNodeKindString(node) << "]"
                            << " (no location info)\n";
            SVFUtil::errs() << "  Node Info: " << node->toString() << "\n";
            return;
        }

        // Add node to the location map
        auto& entry = locationMap[locKey];
        // Only store location object if it's the first node at this location
        if (entry.second.empty()) {
            entry.first = locObj;
        }
        entry.second.push_back({node->getId(), getSVFGNodeKindString(node)});

        SVFUtil::errs() << "  [+] Found Node " << node->getId()
                        << " [" << getSVFGNodeKindString(node) << "]"
                        << " at " << locKey << "\n";
    };

    // Process start node
    processNode(startNode);

    // BFS traversal
    while (!worklist.empty()) {
        const SVFGNode* currentNode = worklist.front();
        worklist.pop();

        for (const SVFGEdge* edge : currentNode->getOutEdges()) {
            const SVFGNode* nextNode = edge->getDstNode();

            // Only traverse within the same function as the start node
            if (nextNode->getFun() == startFunction && visited.find(nextNode) == visited.end()) {
                visited.insert(nextNode);
                worklist.push(nextNode);
                processNode(nextNode);
            }
        }
    }

    // Build result: aggregate by location
    llvm::json::Array locationsArray;
    for (const auto& [locKey, entry] : locationMap) {
        llvm::json::Object locEntry;
        // Copy the location object
        llvm::json::Object locCopy = entry.first;
        locEntry["location"] = std::move(locCopy);
        
        // Build node kind list
        llvm::json::Array nodeKindList;
        for (const auto& [nodeId, nodeKind] : entry.second) {
            llvm::json::Object nodeInfo;
            nodeInfo["node_id"] = nodeId;
            nodeInfo["node_kind"] = nodeKind;
            nodeKindList.push_back(std::move(nodeInfo));
        }
        locEntry["node_kind_list"] = std::move(nodeKindList);
        
        locationsArray.push_back(std::move(locEntry));
    }

    // Count total nodes with valid locations
    size_t nodesWithLocation = 0;
    for (const auto& [locKey, entry] : locationMap) {
        nodesWithLocation += entry.second.size();
    }

    llvm::json::Object result;
    result["start_node_id"] = startNode->getId();
    result["function"] = startFunction->getName();
    result["involved_locations"] = std::move(locationsArray);
    result["total_locations"] = locationMap.size();
    result["total_nodes_with_location"] = nodesWithLocation;
    result["total_nodes_visited"] = visited.size();
    result["nodes_filtered"] = visited.size() - nodesWithLocation;
    result["error"] = false;
    
    SVFUtil::errs() << "--------------------------------------------------\n";
    SVFUtil::errs() << "Summary: " << locationMap.size() << " unique locations, "
                    << nodesWithLocation << " nodes with location, "
                    << (visited.size() - nodesWithLocation) << " nodes filtered (no location)\n";
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}


void PathQuery::dfsVisit(const SVFGNode* currentNode, SVFGPath& currentPath, std::set<const SVFGNode*>& visited) {
    // Add current node to path and visited set
    currentPath.push_back(currentNode);
    visited.insert(currentNode);

    // Check if the current node is a sink (e.g., a call to free)
    if (const ActualParmSVFGNode* paramNode = SVFUtil::dyn_cast<ActualParmSVFGNode>(currentNode)) {
        const CallICFGNode* callSite = paramNode->getCallSite();
        if (callSite) {
            const FunObjVar* callee = callSite->getCalledFunction();
            if (callee && saberApi->isMemDealloc(callee)) {
                printPath(currentPath, true);
                // Path ends here, backtrack
                currentPath.pop_back();
                // Note: We don't remove from 'visited' here to break cycles globally for this traversal branch.
                return;
            }
        }
    }

    bool hasOutEdges = false;
    for (const SVFGEdge* edge : currentNode->getOutEdges()) {
        const SVFGNode* nextNode = edge->getDstNode();
        
        // Avoid cycles
        if (visited.find(nextNode) == visited.end()) {
            hasOutEdges = true;
            dfsVisit(nextNode, currentPath, visited);
        }
    }

    // If it's a leaf node in the traversal and not a sink, it's a potential leak
    if (!hasOutEdges) {
        // We double-check it's not a sink, although the earlier check should cover it.
        bool isSink = false;
        if (const ActualParmSVFGNode* paramNode = SVFUtil::dyn_cast<ActualParmSVFGNode>(currentNode)) {
            const CallICFGNode* callSite = paramNode->getCallSite();
            if (callSite && callSite->getCalledFunction() && saberApi->isMemDealloc(callSite->getCalledFunction())) {
                isSink = true;
            }
        }
        if (!isSink) {
            printPath(currentPath, false);
        }
    }

    // Backtrack
    currentPath.pop_back();
    // To find ALL paths, not just simple paths, we would remove 'currentNode' from 'visited' here.
    // However, for value-flow analysis, finding simple paths is usually sufficient and avoids state explosion.
    // If you need all paths including cycles, you can uncomment the next line:
    // visited.erase(currentNode);
}

void PathQuery::printPath(const SVFGPath& path, bool isFreed) {
    if (isFreed) {
        SVFUtil::outs() << "[Path Found: Safely Freed]\n";
    } else {
        SVFUtil::outs() << "[Path Found: Potential Leak]\n";
    }

    for (size_t i = 0; i < path.size(); ++i) {
        const SVFGNode* node = path[i];
        SVFUtil::outs() << "  " << i << ": "
                        << "Node " << node->getId()
                        << " at " << node->getSourceLoc()
                        << " | " << node->toString() << "\n";
    }
    SVFUtil::outs() << "--------------------------------------------------\n";
}

void PathQuery::getConditionPath(const std::string& startLocation, const std::string& targetLocation)
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
                    newPathEvents.push_back(GraphReaderUtil::formatBranchInfo(intraEdge));
                }
            }
            worklist.emplace_back(succNode, std::move(newPathEvents), std::move(newPathVisited), std::move(newCallStack));
        }
    }

    result["paths"] = std::move(pathsArray);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}

void PathQuery::getConditionInsidePath(const std::string& startLocation, const std::string& targetLocation)
{
    llvm::json::Object result;
    llvm::json::Array pathsArray;

    const ICFGNode* startNode = GraphReaderUtil::findICFGNodeByLocation(icfg, startLocation);
    const ICFGNode* targetNode = GraphReaderUtil::findICFGNodeByLocation(icfg, targetLocation);

    if (!startNode || !targetNode) {
        GraphReaderUtil::sendJsonError("Invalid start or target location.");
        return;
    }

    // Ensure both nodes are within the same function for intra-procedural analysis.
    if (startNode->getFun() != targetNode->getFun()) {
        GraphReaderUtil::sendJsonError("Start and target locations are not in the same function.");
        return;
    }

    // Use a worklist to simulate DFS. We don't need a call stack for intra-procedural analysis.
    // {currentNode, pathEvents, visitedNodes}
    std::vector<std::tuple<const ICFGNode*, llvm::json::Array, Set<const ICFGNode*>>> worklist;
    worklist.emplace_back(startNode, llvm::json::Array{}, Set<const ICFGNode*>{startNode});
    
    int pathIdCounter = 0;

    while (!worklist.empty())
    {
        auto [currentNode, currentPathEvents, pathVisited] = worklist.back();
        worklist.pop_back();

        if (currentNode == targetNode) {
            pathIdCounter++;
            llvm::json::Object pathObject;
            pathObject["path_id"] = pathIdCounter;
            pathObject["events"] = std::move(currentPathEvents);
            pathsArray.push_back(std::move(pathObject));
            continue; 
        }

        for (ICFGEdge* edge : currentNode->getOutEdges())
        {
            ICFGNode* succNode = edge->getDstNode();
            auto newPathEvents = currentPathEvents;
            auto newPathVisited = pathVisited;

            if (const CallCFGEdge* callEdge = SVFUtil::dyn_cast<CallCFGEdge>(edge)) {
                // For a call edge, we don't traverse into the callee.
                // Instead, we jump directly to the return site of this call.
                const CallICFGNode* callSiteNode = callEdge->getCallSite();
                succNode = const_cast<RetICFGNode*>(callSiteNode->getRetICFGNode());

                // Add an event indicating a skipped function call.
                newPathEvents.push_back(llvm::json::Object{
                    {"type", "skipped-call"},
                    {"location", GraphReaderUtil::parseSourceLocation(callSiteNode->getSourceLoc())},
                    {"function", callSiteNode->getCalledFunction() ? callSiteNode->getCalledFunction()->getName() : "indirect_call"}
                });
            }
            else if (SVFUtil::isa<RetCFGEdge>(edge)) {
                // We are analyzing within a single function, so we ignore return edges that lead to callers.
                continue;
            }
            else if (const IntraCFGEdge* intraEdge = SVFUtil::dyn_cast<IntraCFGEdge>(edge)) {
                if (intraEdge->getCondition()) {
                    newPathEvents.push_back(GraphReaderUtil::formatBranchInfo(intraEdge));
                }
            }

            if (newPathVisited.count(succNode)) {
                continue; // Avoid cycles.
            }
            newPathVisited.insert(succNode);

            worklist.emplace_back(succNode, std::move(newPathEvents), std::move(newPathVisited));
        }
    }

    result["paths"] = std::move(pathsArray);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}

void PathQuery::getConstrain(const std::string& location) {
    const ICFGNode* startNode = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
    if (!startNode) {
        GraphReaderUtil::sendJsonError("Invalid location: " + location);
        return;
    }

    // Use BFS to find the nearest conditional branch by traversing backwards.
    std::queue<const ICFGNode*> worklist;
    std::set<const ICFGNode*> visited;

    worklist.push(startNode);
    visited.insert(startNode);

    while (!worklist.empty()) {
        const ICFGNode* currentNode = worklist.front();
        worklist.pop();

        for (const ICFGEdge* edge : currentNode->getInEdges()) {
            const ICFGNode* predNode = edge->getSrcNode();

            // Check if the edge is a conditional branch.
            if (const auto* intraEdge = SVFUtil::dyn_cast<IntraCFGEdge>(edge)) {
                if (intraEdge->getCondition()) {
                    // Found the closest conditional branch.
                    llvm::json::Object result;
                    result["constraint_found"] = true;
                    result["branch_info"] = GraphReaderUtil::formatBranchInfo(intraEdge);
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                    llvm::outs().flush();
                    return;
                }
            }

            // Add predecessor to the worklist if it's in the same function and not visited.
            if (predNode->getFun() == startNode->getFun() && visited.find(predNode) == visited.end()) {
                visited.insert(predNode);
                worklist.push(predNode);
            }
        }
    }

    // If we reach here, no conditional branch was found in the backward slice within the function.
    llvm::json::Object result;
    result["constraint_found"] = false;
    result["message"] = "No preceding conditional branch found within the function.";
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}

void PathQuery::findFunArgValuePathInside(const std::string& funcName, int argIndex) {
    SVFUtil::outs() << "[findFunArgValuePathInside] Function name: " << funcName << ", Arg index: " << argIndex << "\n";
    if (!pag) {
        SVFUtil::errs() << "[findFunArgValuePathInside] PAG is null!\n";
        return;
    }
    // 1. 找到目标函数的 FunObjVar
    const FunObjVar* fun = pag->getFunObjVar(funcName);
    if (!fun) {
        SVFUtil::errs() << "[findFunArgValuePathInside] Function '" << funcName << "' not found in PAG!\n";
        return;
    }
    SVFUtil::outs() << "[findFunArgValuePathInside] FunObjVar ID: " << fun->getId() << ", name: " << fun->getName() << "\n";

    // 2. 获取参数列表
    const SVFIR::SVFVarList& args = pag->getFunArgsList(fun);
    SVFUtil::outs() << "[findFunArgValuePathInside] Function '" << funcName << "' has " << args.size() << " arguments.\n";
    for (size_t i = 0; i < args.size(); ++i) {
        const PAGNode* node = args[i];
        SVFUtil::outs() << "  Arg " << i << ": PAGNode ID = " << node->getId();
        const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(node);
        if (llvmVal) {
            SVFUtil::outs() << ", IR Name = '" << llvmVal->getName().str() << "'\n";
        } else {
            SVFUtil::outs() << ", [No LLVM IR name available]\n";
        }
    }

    if (argIndex < 0 || static_cast<size_t>(argIndex) >= args.size()) {
        SVFUtil::errs() << "[findFunArgValuePathInside] Arg index " << argIndex << " out of range!\n";
        return;
    }
    const PAGNode* targetArg = args[argIndex];
    SVFUtil::outs() << "[findFunArgValuePathInside] Selected Arg index " << argIndex << ", PAGNode ID: " << targetArg->getId() << "\n";
    const llvm::Value* argVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(targetArg);
    if (argVal) {
        SVFUtil::outs() << "  [findFunArgValuePathInside] LLVM Arg Name: '" << argVal->getName().str() << "'\n";
    }

    // 3. 用该PAGNode查找def节点（SVFG中的定义点）
    const SVFGNode* defNode = nullptr;
    if (svfg->hasDefSVFGNode(targetArg)) {
        defNode = svfg->getDefSVFGNode(targetArg);
        SVFUtil::outs() << "[findFunArgValuePathInside] getDefSVFGNode succeeded. Node ID: " << defNode->getId() << "\n";
        SVFUtil::outs() << "  Node Info: " << defNode->toString() << "\n";
    } else {
        SVFUtil::errs() << "[findFunArgValuePathInside] No def SVFGNode for the selected argument!\n";
        // 补充：可以遍历所有节点尝试用 getValue/getParam 比较
    }

    if (!defNode) {
        SVFUtil::errs() << "[findFunArgValuePathInside] Cannot start value path trace due to missing defNode!\n";
        return;
    }
    // 4. 以该defNode为起点，做函数内值流追踪
    SVFUtil::outs() << "[findFunArgValuePathInside] ---- Start intra-procedural value flow BFS ----\n";
    getValueInsidePath(defNode);
    SVFUtil::outs() << "[findFunArgValuePathInside] ---- End BFS ----\n";
}

void PathQuery::findVarValuePathInsideByLocation(const std::string& location, int operandIndex) {
    SVFUtil::outs() << "\n========================================\n";
    SVFUtil::outs() << "[findVarValuePathInsideByLocation] Location: " << location 
                    << ", Operand index: " << operandIndex << "\n";
    SVFUtil::outs() << "========================================\n\n";
    
    if (!icfg || !svfg || !pag) {
        GraphReaderUtil::sendJsonError("ICFG, SVFG, or PAG is null!");
        return;
    }

    // ========== DEBUG 1: Find ALL ICFGNodes at this location ==========
    SVFUtil::outs() << "[DEBUG 1] Finding ALL ICFGNodes at location " << location << "...\n";
    std::vector<const ICFGNode*> allICFGNodes = GraphReaderUtil::findAllICFGNodesByLocation(icfg, location);
    SVFUtil::outs() << "[DEBUG 1] Found " << allICFGNodes.size() << " ICFGNode(s) at this location\n\n";
    
    if (allICFGNodes.empty()) {
        GraphReaderUtil::sendJsonError("Cannot find any ICFGNode for location: " + location);
        return;
    }

    // Process each ICFGNode
    for (size_t icfgIdx = 0; icfgIdx < allICFGNodes.size(); ++icfgIdx) {
        const ICFGNode* icfgNode = allICFGNodes[icfgIdx];
        SVFUtil::outs() << "\n[DEBUG 1] ICFGNode #" << icfgIdx << " (ID: " << icfgNode->getId() << ")\n";
        SVFUtil::outs() << "  Source Location: " << icfgNode->getSourceLoc() << "\n";
        SVFUtil::outs() << "  Type: ";
        if (SVFUtil::isa<IntraICFGNode>(icfgNode)) {
            SVFUtil::outs() << "IntraICFGNode";
        } else if (SVFUtil::isa<CallICFGNode>(icfgNode)) {
            SVFUtil::outs() << "CallICFGNode";
        } else if (SVFUtil::isa<RetICFGNode>(icfgNode)) {
            SVFUtil::outs() << "RetICFGNode";
        } else {
            SVFUtil::outs() << "Other";
        }
        SVFUtil::outs() << "\n";

        // ========== DEBUG 2: Get all LLVM instructions from this ICFGNode ==========
        SVFUtil::outs() << "  [DEBUG 2] Retrieving LLVM instructions from this ICFGNode...\n";
        
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

        if (inst) {
            SVFUtil::outs() << "    Primary LLVM Instruction: ";
            inst->print(llvm::outs());
            SVFUtil::outs() << "\n";
            SVFUtil::outs() << "    Instruction Type: " << llvm::Instruction::getOpcodeName(inst->getOpcode()) << "\n";
            
            // Print all operands
            SVFUtil::outs() << "    Operands (" << inst->getNumOperands() << "):\n";
            for (unsigned i = 0; i < inst->getNumOperands(); ++i) {
                SVFUtil::outs() << "      [" << i << "] ";
                inst->getOperand(i)->print(llvm::outs());
                SVFUtil::outs() << "\n";
            }
            
            // Print result (if any)
            if (!inst->getType()->isVoidTy()) {
                SVFUtil::outs() << "    Result (LHS): ";
                inst->print(llvm::outs());
                SVFUtil::outs() << "\n";
            }
        } else {
            SVFUtil::outs() << "    ⚠ No LLVM instruction found\n";
        }

        // ========== DEBUG 3: Find all PAGNodes/SVFVars associated with this ICFGNode ==========
        SVFUtil::outs() << "  [DEBUG 3] Finding all PAGNodes/SVFVars in this ICFGNode...\n";
        SVFUtil::outs() << "    This ICFGNode has " << icfgNode->getSVFStmts().size() << " SVFStmt(s)\n";
        
        std::set<const PAGNode*> allPAGNodesAtThisLocation;
        
        int stmtIdx = 0;
        for (const SVFStmt* stmt : icfgNode->getSVFStmts()) {
            SVFUtil::outs() << "    [Stmt " << stmtIdx++ << "] EdgeID: " << stmt->getEdgeID() << ", Kind: ";
            
            const SVFVar* lhs = nullptr;
            const SVFVar* rhs = nullptr;
            
            if (const AddrStmt* addr = SVFUtil::dyn_cast<AddrStmt>(stmt)) {
                SVFUtil::outs() << "AddrStmt\n";
                lhs = addr->getLHSVar();
                rhs = addr->getRHSVar();
            } else if (const CopyStmt* copy = SVFUtil::dyn_cast<CopyStmt>(stmt)) {
                SVFUtil::outs() << "CopyStmt\n";
                lhs = copy->getLHSVar();
                rhs = copy->getRHSVar();
            } else if (const LoadStmt* load = SVFUtil::dyn_cast<LoadStmt>(stmt)) {
                SVFUtil::outs() << "LoadStmt\n";
                lhs = load->getLHSVar();
                rhs = load->getRHSVar();
            } else if (const StoreStmt* store = SVFUtil::dyn_cast<StoreStmt>(stmt)) {
                SVFUtil::outs() << "StoreStmt\n";
                lhs = store->getLHSVar();
                rhs = store->getRHSVar();
            } else if (const GepStmt* gep = SVFUtil::dyn_cast<GepStmt>(stmt)) {
                SVFUtil::outs() << "GepStmt\n";
                lhs = gep->getLHSVar();
                rhs = gep->getRHSVar();
            } else {
                SVFUtil::outs() << "OtherStmt\n";
            }
            
            if (lhs) {
                allPAGNodesAtThisLocation.insert(lhs);
                const llvm::Value* lhsVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(lhs);
                SVFUtil::outs() << "      LHS: PAGNode ID=" << lhs->getId();
                if (lhsVal) {
                    SVFUtil::outs() << ", LLVM=";
                    lhsVal->print(llvm::outs());
                }
                SVFUtil::outs() << "\n";
            }
            if (rhs) {
                allPAGNodesAtThisLocation.insert(rhs);
                const llvm::Value* rhsVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(rhs);
                SVFUtil::outs() << "      RHS: PAGNode ID=" << rhs->getId();
                if (rhsVal) {
                    SVFUtil::outs() << ", LLVM=";
                    rhsVal->print(llvm::outs());
                }
                SVFUtil::outs() << "\n";
            }
        }

        // ========== DEBUG 4: For each PAGNode, find its def SVFGNode and check location ==========
        SVFUtil::outs() << "  [DEBUG 4] Finding def SVFGNodes for all PAGNodes at this location...\n";
        SVFUtil::outs() << "    Found " << allPAGNodesAtThisLocation.size() << " unique PAGNode(s)\n\n";
        
        for (const PAGNode* pagNode : allPAGNodesAtThisLocation) {
            SVFUtil::outs() << "    PAGNode ID: " << pagNode->getId() << "\n";
            
            const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(pagNode);
            if (llvmVal) {
                SVFUtil::outs() << "      LLVM Value: ";
                llvmVal->print(llvm::outs());
                SVFUtil::outs() << "\n";
            }
            
            // Check if has def SVFGNode
            if (svfg->hasDefSVFGNode(pagNode)) {
                const SVFGNode* defNode = svfg->getDefSVFGNode(pagNode);
                SVFUtil::outs() << "      ✓ Has def SVFGNode: ID=" << defNode->getId() 
                                << " [" << getSVFGNodeKindString(defNode) << "]\n";
                SVFUtil::outs() << "      Def Node Location: " << defNode->getSourceLoc() << "\n";
                
                // Parse location to check if it's at line 1098
                llvm::json::Object defLoc = GraphReaderUtil::parseSourceLocation(defNode->getSourceLoc());
                if (auto file = defLoc.getString("fl")) {
                    if (auto line = defLoc.getInteger("ln")) {
                        SVFUtil::outs() << "      Def Node File: " << file->str() << ", Line: " << *line << "\n";
                        if (*line == 1098) {
                            SVFUtil::outs() << "      *** ⭐ THIS DEF NODE IS AT LINE 1098! ⭐ ***\n";
                        }
                    }
                }
                SVFUtil::outs() << "      Def Node Info: " << defNode->toString() << "\n";
            } else {
                SVFUtil::outs() << "      ✗ No def SVFGNode\n";
            }
            SVFUtil::outs() << "\n";
        }
    }

    // Continue with original logic using the first ICFGNode (for backward compatibility)
    SVFUtil::outs() << "\n[INFO] Using first ICFGNode (ID: " << allICFGNodes[0]->getId() << ") for actual value flow analysis...\n\n";
    const ICFGNode* icfgNode = allICFGNodes[0];

    // 2. Get LLVM instruction from ICFGNode
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

    if (!inst) {
        GraphReaderUtil::sendJsonError("Could not retrieve LLVM instruction from ICFGNode at " + location);
        return;
    }
    SVFUtil::outs() << "[findVarValuePathInsideByLocation] Retrieved LLVM instruction\n";

    // 3. Determine target LLVM value based on operand index
    const llvm::Value* targetLLVMValue = nullptr;
    if (operandIndex == -1) { // LHS (defined value)
        if (inst->getType()->isVoidTy()) {
            GraphReaderUtil::sendJsonError("Instruction at " + location + " does not define a value (e.g., store, free). Try specifying an operand index like 0, 1, etc.");
            return;
        }
        targetLLVMValue = inst;
        SVFUtil::outs() << "[findVarValuePathInsideByLocation] Targeting LHS (defined value)\n";
    } else { // RHS operand
        if (operandIndex < 0 || static_cast<unsigned>(operandIndex) >= inst->getNumOperands()) {
            GraphReaderUtil::sendJsonError("Invalid operand index " + std::to_string(operandIndex) + 
                                          " for instruction at " + location + 
                                          ". It has " + std::to_string(inst->getNumOperands()) + " operands.");
            return;
        }
        targetLLVMValue = inst->getOperand(operandIndex);
        SVFUtil::outs() << "[findVarValuePathInsideByLocation] Targeting operand " << operandIndex << "\n";
    }

    if (!targetLLVMValue) {
        GraphReaderUtil::sendJsonError("Could not determine target LLVM value for operand " + std::to_string(operandIndex));
        return;
    }

    // Debug: Print target LLVM value info
    SVFUtil::outs() << "[findVarValuePathInsideByLocation] Target LLVM Value: ";
    targetLLVMValue->print(llvm::outs());
    SVFUtil::outs() << "\n";
    if (targetLLVMValue->hasName()) {
        SVFUtil::outs() << "[findVarValuePathInsideByLocation] LLVM Value Name: '" << targetLLVMValue->getName().str() << "'\n";
    }

    // 4. Find corresponding PAGNode/SVFVar
    const PAGNode* targetPAGNode = nullptr;
    
    SVFUtil::outs() << "[findVarValuePathInsideByLocation] ICFGNode has " << icfgNode->getSVFStmts().size() << " SVFStmts\n";
    
    // Iterate through all SVFStmts in the ICFGNode to find the one that matches our target value
    int stmtIdx = 0;
    for (const SVFStmt* stmt : icfgNode->getSVFStmts()) {
        SVFUtil::outs() << "  [Stmt " << stmtIdx++ << "] EdgeID: " << stmt->getEdgeID() << ", Kind: ";
        
        // Check if this statement involves our target value
        const SVFVar* lhs = nullptr;
        const SVFVar* rhs = nullptr;
        
        if (const AddrStmt* addr = SVFUtil::dyn_cast<AddrStmt>(stmt)) {
            SVFUtil::outs() << "AddrStmt";
            lhs = addr->getLHSVar();
            rhs = addr->getRHSVar();
        } else if (const CopyStmt* copy = SVFUtil::dyn_cast<CopyStmt>(stmt)) {
            SVFUtil::outs() << "CopyStmt";
            lhs = copy->getLHSVar();
            rhs = copy->getRHSVar();
        } else if (const LoadStmt* load = SVFUtil::dyn_cast<LoadStmt>(stmt)) {
            SVFUtil::outs() << "LoadStmt";
            lhs = load->getLHSVar();
            rhs = load->getRHSVar();
        } else if (const StoreStmt* store = SVFUtil::dyn_cast<StoreStmt>(stmt)) {
            SVFUtil::outs() << "StoreStmt";
            lhs = store->getLHSVar();
            rhs = store->getRHSVar();
        } else if (const GepStmt* gep = SVFUtil::dyn_cast<GepStmt>(stmt)) {
            SVFUtil::outs() << "GepStmt";
            lhs = gep->getLHSVar();
            rhs = gep->getRHSVar();
        } else {
            SVFUtil::outs() << "OtherStmt";
        }
        SVFUtil::outs() << "\n";
        
        // Check if LHS or RHS matches our target (by comparing LLVM values)
        if (lhs) {
            const llvm::Value* lhsVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(lhs);
            SVFUtil::outs() << "    LHS: PAGNode ID=" << lhs->getId();
            if (lhsVal) {
                SVFUtil::outs() << ", LLVM=";
                lhsVal->print(llvm::outs());
            }
            SVFUtil::outs() << "\n";
            
            if (lhsVal == targetLLVMValue) {
                SVFUtil::outs() << "    *** MATCH FOUND on LHS! ***\n";
                targetPAGNode = lhs;
                break;
            }
        }
        if (rhs) {
            const llvm::Value* rhsVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(rhs);
            SVFUtil::outs() << "    RHS: PAGNode ID=" << rhs->getId();
            if (rhsVal) {
                SVFUtil::outs() << ", LLVM=";
                rhsVal->print(llvm::outs());
            }
            SVFUtil::outs() << "\n";
            
            if (rhsVal == targetLLVMValue) {
                SVFUtil::outs() << "    *** MATCH FOUND on RHS! ***\n";
                targetPAGNode = rhs;
                break;
            }
        }
    }

    if (!targetPAGNode) {
        GraphReaderUtil::sendJsonError("Could not find PAGNode for the target value at " + location);
        SVFUtil::errs() << "[findVarValuePathInsideByLocation] This might happen if the value is a constant or not represented in PAG.\n";
        return;
    }
    
    SVFUtil::outs() << "[findVarValuePathInsideByLocation] Found PAGNode ID: " << targetPAGNode->getId() << "\n";

    // 5. Get def SVFGNode for this PAGNode
    SVFUtil::outs() << "[findVarValuePathInsideByLocation] Checking if PAGNode " << targetPAGNode->getId() << " has def SVFGNode...\n";
    
    const SVFGNode* defNode = nullptr;
    if (svfg->hasDefSVFGNode(targetPAGNode)) {
        defNode = svfg->getDefSVFGNode(targetPAGNode);
        SVFUtil::outs() << "[findVarValuePathInsideByLocation] ✓ Found def SVFGNode ID: " << defNode->getId() << "\n";
        SVFUtil::outs() << "  Node Kind: " << getSVFGNodeKindString(defNode) << "\n";
        SVFUtil::outs() << "  Node Info: " << defNode->toString() << "\n";
        SVFUtil::outs() << "  Node Function: " << (defNode->getFun() ? defNode->getFun()->getName() : "<no function>") << "\n";
    } else {
        SVFUtil::errs() << "[findVarValuePathInsideByLocation] ✗ No def SVFGNode found for PAGNode " << targetPAGNode->getId() << "\n";
        
        // Additional debug: Check if this PAGNode appears in any SVFGNode
        SVFUtil::errs() << "[findVarValuePathInsideByLocation] Searching for any SVFGNode that references this PAGNode...\n";
        int foundCount = 0;
        for (auto it = svfg->begin(); it != svfg->end(); ++it) {
            const SVFGNode* node = it->second;
            
            // Check various node types
            bool matches = false;
            if (const StmtVFGNode* stmtNode = SVFUtil::dyn_cast<StmtVFGNode>(node)) {
                if (stmtNode->getPAGSrcNode() == targetPAGNode || stmtNode->getPAGDstNode() == targetPAGNode) {
                    matches = true;
                }
            } else if (const ActualParmVFGNode* apNode = SVFUtil::dyn_cast<ActualParmVFGNode>(node)) {
                if (apNode->getParam() == targetPAGNode) {
                    matches = true;
                }
            } else if (const FormalParmVFGNode* fpNode = SVFUtil::dyn_cast<FormalParmVFGNode>(node)) {
                if (fpNode->getParam() == targetPAGNode) {
                    matches = true;
                }
            }
            
            if (matches && foundCount < 5) { // Limit output to first 5 matches
                SVFUtil::errs() << "  Found in SVFGNode ID=" << node->getId() 
                                << " [" << getSVFGNodeKindString(node) << "]\n";
                SVFUtil::errs() << "    " << node->toString() << "\n";
                foundCount++;
            }
        }
        
        if (foundCount == 0) {
            SVFUtil::errs() << "[findVarValuePathInsideByLocation] PAGNode " << targetPAGNode->getId() 
                            << " is not referenced by any SVFGNode!\n";
        } else {
            SVFUtil::errs() << "[findVarValuePathInsideByLocation] Found " << foundCount 
                            << " SVFGNode(s) referencing this PAGNode, but none is marked as def.\n";
        }
        
        GraphReaderUtil::sendJsonError("No def SVFGNode found for the target variable at " + location);
        return;
    }

    // 6. Perform intra-procedural value flow analysis from this def node
    SVFUtil::outs() << "[findVarValuePathInsideByLocation] ---- Start intra-procedural value flow BFS ----\n";
    getValueInsidePath(defNode);
    SVFUtil::outs() << "[findVarValuePathInsideByLocation] ---- End BFS ----\n";
}

void PathQuery::findLVarPathInsideByLocation(const std::string& location, int eqPosition) {
    SVFUtil::outs() << "\n========================================\n";
    SVFUtil::outs() << "[findLVarPathInsideByLocation] Location: " << location 
                    << ", Eq position: " << eqPosition << "\n";
    SVFUtil::outs() << "========================================\n\n";
    
    if (!icfg || !svfg || !pag) {
        GraphReaderUtil::sendJsonError("ICFG, SVFG, or PAG is null!");
        return;
    }

    // ========== DEBUG 1: Find ALL ICFGNodes at this location ==========
    SVFUtil::outs() << "[DEBUG 1] Finding ALL ICFGNodes at location " << location << "...\n";
    std::vector<const ICFGNode*> allICFGNodes = GraphReaderUtil::findAllICFGNodesByLocation(icfg, location);


    int idx = 0;
    for (size_t i = 0; i < allICFGNodes.size(); i++) {
        const ICFGNode* icfgNode = allICFGNodes[i];
        SVFUtil::outs() << "[DEBUG 1] ICFGNode ID: " << icfgNode->getId() << "\n";
        SVFUtil::outs() << "[DEBUG 1] ICFGNode Source Location: " << icfgNode->getSourceLoc() << "\n";
        SVFUtil::outs() << "[DEBUG 1] ICFGNode Type: " << icfgNode->toString() << "\n";
        // 如果icfgnode的类型是IntraICFGNode
        if (SVFUtil::isa<IntraICFGNode>(icfgNode)) {
            // 得到他的sourcelocation
            const std::string sourceLocation = icfgNode->getSourceLoc();
            // cl字段
            SVFUtil::outs() << "    sourceLocation: " << sourceLocation << "\n";
            llvm::json::Object locObj = GraphReaderUtil::parseSourceLocation(sourceLocation);
            //SVFUtil::outs() << "    locObj: " << llvm::formatv("{0}", llvm::json::Value(llvm::json::Object(locObj))) << "\n";
            if (locObj.empty()) {
                SVFUtil::outs() << "    locObj is empty\n";
                continue;
            }
            if (auto cl = locObj.getInteger("cl")) {
                if (cl == eqPosition) {
                    // 获取当前的icfg在哪一个index
                    idx = i;
                    break;
                }
            }
        }
    }
    const ICFGNode* targetICFGNode = allICFGNodes[idx];
    //

    auto intraNode = SVFUtil::dyn_cast<IntraICFGNode>(targetICFGNode);
    if (!intraNode) {
        GraphReaderUtil::sendJsonError("Cannot find IntraICFGNode for location: " + location);
        return;
    } else {
        SVFUtil::outs() << "    IntraICFGNode found for location: " << location << "\n";
        SVFUtil::outs() << "    IntraICFGNode debug info: " << targetICFGNode->toString() << "\n";
    }
    const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(intraNode);
    const llvm::Instruction* inst = SVFUtil::dyn_cast<llvm::Instruction>(llvmVal);
    if (!inst) {
        GraphReaderUtil::sendJsonError("Cannot find LLVM instruction for IntraICFGNode: " + location);
        return;
    }
    if (inst) {
        SVFUtil::outs() << "    Primary LLVM Instruction: ";
        inst->print(llvm::outs());
        SVFUtil::outs() << "\n";
        SVFUtil::outs() << "    Instruction Type: " << llvm::Instruction::getOpcodeName(inst->getOpcode()) << "\n";
        
        // Print all operands
        SVFUtil::outs() << "    Operands (" << inst->getNumOperands() << "):\n";
        for (unsigned i = 0; i < inst->getNumOperands(); ++i) {
            SVFUtil::outs() << "      [" << i << "] ";
            inst->getOperand(i)->print(llvm::outs());
            SVFUtil::outs() << "\n";
        }
        
        // Print result (if any)
        if (!inst->getType()->isVoidTy()) {
            SVFUtil::outs() << "    Result (LHS): ";
            inst->print(llvm::outs());
            SVFUtil::outs() << "\n";
        }
    } else {
        SVFUtil::outs() << "    ⚠ No LLVM instruction found\n";
    }
    SVFUtil::outs() << "  [DEBUG 3] Finding all PAGNodes/SVFVars in this ICFGNode...\n";
    SVFUtil::outs() << "    This ICFGNode has " << targetICFGNode->getSVFStmts().size() << " SVFStmt(s)\n";    
    int stmtIdx = 0;
    for (const SVFStmt* stmt : targetICFGNode->getSVFStmts()) {
        SVFUtil::outs() << "    [Stmt " << stmtIdx++ << "] EdgeID: " << stmt->getEdgeID() << ", Kind: ";
        SVFUtil::outs() << stmt->toString() << "\n";
    }

    //一般只会有一个SVFStmt，有多条的场景小意味着找错了 很可能遇到了OtherStmt 
    //忽略其他找到多条的场景 取第一条 拿到左值PAG节点
    if (targetICFGNode->getSVFStmts().empty()) {
        GraphReaderUtil::sendJsonError("No SVF statements found for the ICFG node at location: " + location);
        return;
    }
    const SVFStmt* targetStmt = targetICFGNode->getSVFStmts().front();
    const SVFVar* lhs = nullptr;
    const SVFVar* rhs = nullptr;
    if (const AddrStmt* addr = SVFUtil::dyn_cast<AddrStmt>(targetStmt)) {
        lhs = addr->getLHSVar();
        rhs = addr->getRHSVar();
    } else if (const CopyStmt* copy = SVFUtil::dyn_cast<CopyStmt>(targetStmt)) {
        lhs = copy->getLHSVar();
        rhs = copy->getRHSVar();
    } else if (const LoadStmt* load = SVFUtil::dyn_cast<LoadStmt>(targetStmt)) {
        lhs = load->getLHSVar();
        rhs = load->getRHSVar();
    } else if (const StoreStmt* store = SVFUtil::dyn_cast<StoreStmt>(targetStmt)) {
        lhs = store->getLHSVar();
        rhs = store->getRHSVar();
    } else if (const GepStmt* gep = SVFUtil::dyn_cast<GepStmt>(targetStmt)) {
        lhs = gep->getLHSVar();
        rhs = gep->getRHSVar();
    } else {
        GraphReaderUtil::sendJsonError("The first SVF statement is not an AddrStmt, CopyStmt, LoadStmt, StoreStmt, or GepStmt at location: " + location);
        return;
    }

    if (lhs) {
        SVFUtil::outs() << "    LHS: PAGNode ID=" << lhs->getId() << "\n";
    }
    if (rhs) {
        SVFUtil::outs() << "    RHS: PAGNode ID=" << rhs->getId() << "\n";
    }
    const SVFGNode* defNode = nullptr;
    if (lhs) {
        if (svfg->hasDefSVFGNode(lhs)) {
            defNode = svfg->getDefSVFGNode(lhs);
            SVFUtil::outs() << "    Def SVFGNode ID=" << defNode->getId() << "\n";
        } else {
            SVFUtil::outs() << "    No def SVFGNode found for LHS PAGNode ID=" << lhs->getId() << "\n";
        }
    }
    // 6. Perform intra-procedural value flow analysis from this def node
    SVFUtil::outs() << "[findLVarPathInsideByLocation] ---- Start intra-procedural value flow BFS ----\n";
    getValueInsidePath(defNode);
    SVFUtil::outs() << "[findLVarPathInsideByLocation] ---- End BFS ----\n";    
} 
}// namespace SVF
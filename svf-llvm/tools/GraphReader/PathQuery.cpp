#include "PathQuery.h"
#include "GraphReaderUtil.h"
#include "Graphs/ICFG.h"
#include "Graphs/SVFGEdge.h"
#include "SVFIR/SVFValue.h"
#include "Graphs/VFGNode.h"
#include "Graphs/SVFG.h"
#include "Util/SVFUtil.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/SVFVariables.h"
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <deque>
#include <iostream>
#include <map>
#include <queue>
#include <set>

namespace SVF {

void PathQuery::getValuePath(const SVFGNode* startNode) {
    // DEBUG
    // old implementation
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
    
    // Map to track predecessors for path reconstruction
    std::map<const SVFGNode*, const SVFGNode*> predecessorMap;

    // Start BFS from the initial node
    worklist.push(startNode);
    visited.insert(startNode);
    predecessorMap[startNode] = nullptr; // Start node has no predecessor

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
                            << " [" << GraphReaderUtil::getSVFGNodeKindString(node) << "]"
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
        entry.second.push_back({node->getId(), GraphReaderUtil::getSVFGNodeKindString(node)});

        SVFUtil::errs() << "  [+] Found Node " << node->getId()
                        << " [" << GraphReaderUtil::getSVFGNodeKindString(node) << "]"
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
                predecessorMap[nextNode] = currentNode; // Record predecessor
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
    
    // Build paths: reconstruct paths from start node to each visited node
    llvm::json::Array pathsArray;
    for (const SVFGNode* node : visited) {
        // Reconstruct path from start node to current node
        std::vector<const SVFGNode*> pathNodes;
        const SVFGNode* current = node;
        
        while (current != nullptr) {
            pathNodes.push_back(current);
            auto it = predecessorMap.find(current);
            if (it != predecessorMap.end()) {
                current = it->second;
            } else {
                break;
            }
        }
        
        // Reverse to get path from start to end
        std::reverse(pathNodes.begin(), pathNodes.end());
        
        // Build JSON array for this path
        llvm::json::Array singlePath;
        for (const SVFGNode* pathNode : pathNodes) {
            llvm::json::Object nodeObj;
            nodeObj["node_id"] = pathNode->getId();
            nodeObj["node_kind"] = GraphReaderUtil::getSVFGNodeKindString(pathNode);
            singlePath.push_back(std::move(nodeObj));
        }
        
        pathsArray.push_back(std::move(singlePath));
    }

    // Count total nodes with valid locations
    size_t nodesWithLocation = 0;
    for (const auto& [locKey, entry] : locationMap) {
        nodesWithLocation += entry.second.size();
    }
    
    // Save path count before moving
    size_t pathCount = pathsArray.size();

    llvm::json::Object result;
    result["start_node_id"] = startNode->getId();
    result["function"] = startFunction->getName();
    result["involved_locations"] = std::move(locationsArray);
    result["total_locations"] = locationMap.size();
    result["total_nodes_with_location"] = nodesWithLocation;
    result["total_nodes_visited"] = visited.size();
    result["nodes_filtered"] = visited.size() - nodesWithLocation;
    
    // Add path information
    llvm::json::Object pathInfo;
    pathInfo["pathcount"] = static_cast<int64_t>(pathCount);
    pathInfo["paths"] = std::move(pathsArray);
    result["path_info"] = std::move(pathInfo);
    
    result["error"] = false;
    
    SVFUtil::errs() << "--------------------------------------------------\n";
    SVFUtil::errs() << "Summary: " << locationMap.size() << " unique locations, "
                    << nodesWithLocation << " nodes with location, "
                    << (visited.size() - nodesWithLocation) << " nodes filtered (no location), "
                    << pathCount << " paths found\n";
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}


void PathQuery::dfsVisit(const SVFGNode* currentNode, SVFGPath& currentPath, std::set<const SVFGNode*>& visited) {
    // DEBUG
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
    // TOOL FUNCTION
    // old implementation
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
    // TOOL FUNCTION
    // old implementation
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
    // DEBUG
    // maybe useful
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
    // DEBUG
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
    // DEBUG
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
                                << " [" << GraphReaderUtil::getSVFGNodeKindString(defNode) << "]\n";
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
        SVFUtil::outs() << "  Node Kind: " << GraphReaderUtil::getSVFGNodeKindString(defNode) << "\n";
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
                                << " [" << GraphReaderUtil::getSVFGNodeKindString(node) << "]\n";
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
    // DEBUG
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

// Helper function to find actual return statement ICFG nodes (not wrapper nodes)
std::vector<const ICFGNode*> findActualReturnICFGNodes(ICFG* icfg, const FunObjVar* function) {
    // TOOL FUNCTION
    std::vector<const ICFGNode*> actualReturnNodes;
    if (!icfg) {
        return actualReturnNodes;
    }

    std::vector<const IntraICFGNode*> retInstNodes;
    for (ICFG::const_iterator it = icfg->begin(), eit = icfg->end(); it != eit; ++it) {
        const ICFGNode* node = it->second;
        if (node->getFun() != function) {
            continue;
        }
        if (const auto* intraNode = SVFUtil::dyn_cast<IntraICFGNode>(node)) {
            if (intraNode->isRetInst()) {
                retInstNodes.push_back(intraNode);
            }
        }
    }

    if (retInstNodes.empty()) {
        return actualReturnNodes;
    }

    auto collectPredecessors = [](const std::vector<const IntraICFGNode*>& nodes) {
        std::vector<const IntraICFGNode*> predecessors;
        for (const auto* node : nodes) {
            for (const ICFGEdge* edge : node->getInEdges()) {
                if (const auto* src = SVFUtil::dyn_cast<IntraICFGNode>(edge->getSrcNode())) {
                    predecessors.push_back(src);
                }
            }
        }
        return predecessors;
    };

    auto collectUnconditionalBranches = [](const std::vector<const IntraICFGNode*>& nodes) {
        std::vector<const IntraICFGNode*> branchNodes;
        for (const auto* node : nodes) {
            for (const SVFStmt* stmt : node->getSVFStmts()) {
                if (const auto* branchStmt = SVFUtil::dyn_cast<BranchStmt>(stmt)) {
                    if (branchStmt->isUnconditional()) {
                        branchNodes.push_back(node);
                        break;
                    }
                }
            }
        }
        return branchNodes;
    };

    auto collectLoadNodes = [](const std::vector<const IntraICFGNode*>& nodes) {
        std::vector<const IntraICFGNode*> loadNodes;
        for (const auto* node : nodes) {
            for (const SVFStmt* stmt : node->getSVFStmts()) {
                if (SVFUtil::isa<LoadStmt>(stmt)) {
                    loadNodes.push_back(node);
                    break;
                }
            }
        }
        return loadNodes;
    };

    Set<NodeID> seenReturnIds;
    auto appendUnique = [&](const std::vector<const IntraICFGNode*>& nodes) {
        for (const auto* node : nodes) {
            if (node && seenReturnIds.insert(node->getId()).second) {
                actualReturnNodes.push_back(node);
            }
        }
    };

    const bool isVoidFunction = [&]() {
        if (!function) {
            return false;
        }
        if (const llvm::Value* funVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(function)) {
            if (const auto* llvmFunc = llvm::dyn_cast<llvm::Function>(funVal)) {
                return llvmFunc->getReturnType()->isVoidTy();
            }
        }
        return false;
    }();

    const auto srcNodes = collectPredecessors(retInstNodes);
    if (isVoidFunction) {
        const auto branchNodes = collectUnconditionalBranches(srcNodes);
        if (!branchNodes.empty()) {
            appendUnique(branchNodes);
            return actualReturnNodes;
        }
    }

    const auto loadNodes = collectLoadNodes(srcNodes);
    const auto loadSrcNodes = collectPredecessors(loadNodes);
    const auto branchNodes = collectUnconditionalBranches(loadSrcNodes);
    if (!branchNodes.empty()) {
        // 在这里先检查一下所有branchNodes是否都具有location信息 只有有任何一个没有 也要使用retInstNodes
        for (const auto* node : branchNodes) {
            if (node->getSourceLoc().empty()) {
                appendUnique(retInstNodes);
                return actualReturnNodes;
            }
        }
        appendUnique(branchNodes);
        return actualReturnNodes;
    }

    appendUnique(retInstNodes);
    return actualReturnNodes;
}

// Helper function to check if an ICFG node can reach target return location
bool canICFGReach(const ICFGNode* from, const ICFGNode* target, const FunObjVar* function, bool debug = false) {
    // DEBUG
    if (!from || !target) return false;
    if (from == target) return true;
    
    std::queue<const ICFGNode*> worklist;
    Set<const ICFGNode*> visited;
    
    worklist.push(from);
    visited.insert(from);
    
    int steps = 0;
    const int maxSteps = 10000;  // Prevent infinite loops
    
    while (!worklist.empty() && steps < maxSteps) {
        const ICFGNode* current = worklist.front();
        worklist.pop();
        steps++;
        
        if (current == target) {
            if (debug) {
                SVFUtil::outs() << "      [canICFGReach] Found target after " << steps << " steps\n";
            }
            return true;
        }
        
        int outEdgeCount = 0;
        for (const ICFGEdge* edge : current->getOutEdges()) {
            const ICFGNode* next = edge->getDstNode();
            outEdgeCount++;
            
            // Skip call edges into callees (intra-procedural only)
            if (SVFUtil::isa<CallCFGEdge>(edge)) {
                // Jump to the return site instead
                if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(current)) {
                    const RetICFGNode* retNode = callNode->getRetICFGNode();
                    if (retNode && retNode->getFun() == function && !visited.count(retNode)) {
                        visited.insert(retNode);
                        worklist.push(retNode);
                    }
                }
                continue;
            }
            
            // Skip return edges to callers
            if (SVFUtil::isa<RetCFGEdge>(edge)) {
                continue;
            }
            
            // Only traverse within the same function
            if (next->getFun() != function) continue;
            if (visited.count(next)) continue;
            
            visited.insert(next);
            worklist.push(next);
        }
        
        if (debug && steps <= 5) {
            SVFUtil::outs() << "      [canICFGReach] Step " << steps << ": ICFG " << current->getId() 
                            << " has " << outEdgeCount << " out edges, worklist size: " << worklist.size() << "\n";
        }
    }
    
    if (debug) {
        SVFUtil::outs() << "      [canICFGReach] Target not found after " << steps << " steps (visited " << visited.size() << " nodes)\n";
    }
    
    return false;
}

// Helper function to find all ICFG paths from start to target (intra-procedural)
void findICFGPaths(
    const ICFGNode* startICFG,
    const ICFGNode* targetICFG,
    const FunObjVar* function,
    std::vector<std::vector<const ICFGNode*>>& allICFGPaths
) {
    // TOOL FUNCTION
    // Use BFS to find paths (prioritizes shorter paths over DFS)
    std::deque<std::tuple<const ICFGNode*, std::vector<const ICFGNode*>, Set<const ICFGNode*>>> worklist;
    std::vector<const ICFGNode*> initialPath;
    initialPath.push_back(startICFG);
    worklist.emplace_back(startICFG, initialPath, Set<const ICFGNode*>{startICFG});
    
    // Add limits to prevent infinite loops and focus on simple paths
    const size_t MAX_PATHS = 50;  // Maximum number of paths to find (focus on simple paths)
    const size_t MAX_PATH_LENGTH = 100;  // Maximum length of a single path (shorter = simpler)
    const size_t MAX_ITERATIONS = 50000;  // Maximum iterations
    
    size_t iterations = 0;
    size_t lastReportIteration = 0;
    
    while (!worklist.empty()) {
        iterations++;
        
        // Progress reporting every 5000 iterations (more frequent for faster feedback)
        if (iterations - lastReportIteration >= 5000) {
            // SVFUtil::outs() << "  [findICFGPaths] Progress: " << iterations 
            //                 << " iterations, worklist size: " << worklist.size()
            //                 << ", paths found: " << allICFGPaths.size() << "\n";
            lastReportIteration = iterations;
        }
        
        // Check iteration limit
        if (iterations > MAX_ITERATIONS) {
            // SVFUtil::outs() << "  [findICFGPaths] Stopped: Reached max iterations (" 
            //                 << MAX_ITERATIONS << "), found " << allICFGPaths.size() << " paths.\n";
            break;
        }
        
        // Check path count limit - stop early to focus on simpler paths
        if (allICFGPaths.size() >= MAX_PATHS) {
            // SVFUtil::outs() << "  [findICFGPaths] Stopped: Found " << MAX_PATHS 
            //                 << " paths (limit reached).\n";
            break;
        }
        
        // BFS: take from front instead of back (this prioritizes shorter paths)
        auto [currentNode, currentPath, pathVisited] = worklist.front();
        worklist.pop_front();
        
        // Check path length limit
        if (currentPath.size() > MAX_PATH_LENGTH) {
            continue;  // Skip this path
        }
        
        if (currentNode == targetICFG) {
            allICFGPaths.push_back(currentPath);
            continue;
        }
        
        for (ICFGEdge* edge : currentNode->getOutEdges()) {
            ICFGNode* succNode = edge->getDstNode();
            
            // Handle function calls: skip into callee, jump to return site
            if (const CallCFGEdge* callEdge = SVFUtil::dyn_cast<CallCFGEdge>(edge)) {
                const CallICFGNode* callSiteNode = callEdge->getCallSite();
                succNode = const_cast<RetICFGNode*>(callSiteNode->getRetICFGNode());
            }
            else if (SVFUtil::isa<RetCFGEdge>(edge)) {
                // Ignore return edges that lead to callers (intra-procedural only)
                continue;
            }
            
            // Only traverse within the same function
            if (succNode->getFun() != function) {
                continue;
            }
            
            if (pathVisited.count(succNode)) {
                continue;
            }
            
            auto newPath = currentPath;
            newPath.push_back(succNode);
            auto newVisited = pathVisited;
            newVisited.insert(succNode);
            
            worklist.emplace_back(succNode, std::move(newPath), std::move(newVisited));
        }
    }
    
    // if (iterations > lastReportIteration) {
    //     SVFUtil::outs() << "  [findICFGPaths] Completed: " << iterations 
    //                     << " total iterations, found " << allICFGPaths.size() << " paths\n";
    // }
}

void PathQuery::dfsToReturnLocation(
    const SVFGNode* currentNode,
    const ICFGNode* targetReturnICFG,
    const FunObjVar* function,
    std::vector<const SVFGNode*>& currentPath,
    std::set<const SVFGNode*>& visited,
    std::vector<std::vector<const SVFGNode*>>& allPaths
) {
    // DEBUG
    // Check if current SVFG node's ICFG node can reach the target return location
    const ICFGNode* curICFG = currentNode->getICFGNode();
    
    // Debug output
    if (currentPath.size() <= 3) {  // Only log first few steps to avoid spam
        SVFUtil::outs() << "    [DFS] Current SVFG Node " << currentNode->getId() 
                        << " [" << GraphReaderUtil::getSVFGNodeKindString(currentNode) << "]";
        if (curICFG) {
            SVFUtil::outs() << " -> ICFG " << curICFG->getId();
        } else {
            SVFUtil::outs() << " -> No ICFG";
        }
        SVFUtil::outs() << "\n";
    }
    
    // If current ICFG node matches the target, we've reached the return location
    if (curICFG && (curICFG == targetReturnICFG || curICFG->getId() == targetReturnICFG->getId())) {
        SVFUtil::outs() << "    [DFS] ✓ Reached target! Path length: " << currentPath.size() << "\n";
        std::vector<const SVFGNode*> completePath = currentPath;
        allPaths.push_back(completePath);
        return;
    }
    
    // Early pruning: if current ICFG cannot reach target at ICFG level, skip this branch
    if (curICFG && !canICFGReach(curICFG, targetReturnICFG, function)) {
        if (currentPath.size() <= 3) {
            SVFUtil::outs() << "    [DFS] ✗ Cannot reach target from ICFG " << curICFG->getId() << ", pruning\n";
        }
        return;
    }

    // Mark current node as visited
    visited.insert(currentNode);

    // Check if current node is MIntraPhi - if so, add to path
    bool isMIntraPhi = SVFUtil::isa<IntraMSSAPHISVFGNode>(currentNode);
    if (isMIntraPhi && currentNode != currentPath.front()) {
        // Add MIntraPhi to path (not if it's the source node itself)
        currentPath.push_back(currentNode);
    }

    // Traverse outgoing edges
    int edgeCount = 0;
    for (const SVFGEdge* edge : currentNode->getOutEdges()) {
        const SVFGNode* nextNode = edge->getDstNode();
        edgeCount++;

        // Only traverse within the same function
        if (nextNode->getFun() != function) {
            continue;
        }

        // Avoid cycles
        if (visited.find(nextNode) != visited.end()) {
            continue;
        }

        // Recursively explore
        dfsToReturnLocation(nextNode, targetReturnICFG, function, currentPath, visited, allPaths);
    }
    
    if (currentPath.size() <= 3 && edgeCount == 0) {
        SVFUtil::outs() << "    [DFS] Dead end: no outgoing edges\n";
    }

    // Backtrack: remove current node from path if it was added
    if (isMIntraPhi && currentNode != currentPath.front()) {
        currentPath.pop_back();
    }

    // Remove from visited to allow other paths through this node
    visited.erase(currentNode);
}

void PathQuery::findPathsToFormalOUT(const std::string& location, int eqPosition) {
    // DEBUG
    SVFUtil::outs() << "\n========================================\n";
    SVFUtil::outs() << "[findPathsToFormalOUT] Location: " << location 
                    << ", Eq position: " << eqPosition << "\n";
    SVFUtil::outs() << "========================================\n\n";
    
    if (!icfg || !svfg || !pag) {
        GraphReaderUtil::sendJsonError("ICFG, SVFG, or PAG is null!");
        return;
    }

    // Step 1: Find the starting SVFG node at the current location (not the def location)
    SVFUtil::outs() << "[Step 1] Finding starting node at location " << location << "...\n";
    std::vector<const ICFGNode*> allICFGNodes = GraphReaderUtil::findAllICFGNodesByLocation(icfg, location);

    if (allICFGNodes.empty()) {
        GraphReaderUtil::sendJsonError("Cannot find any ICFGNode for location: " + location);
        return;
    }

    // Find the ICFGNode matching the eqPosition
    SVFUtil::outs() << "  Searching for ICFG node matching eqPosition=" << eqPosition << "\n";
    SVFUtil::outs() << "  Total candidate ICFG nodes: " << allICFGNodes.size() << "\n";
    
    const ICFGNode* targetICFGNode = nullptr;
    std::vector<const ICFGNode*> matchedNodes;  // Collect all matches
    
    for (size_t i = 0; i < allICFGNodes.size(); i++) {
        const ICFGNode* icfgNode = allICFGNodes[i];
        SVFUtil::outs() << "  [" << i << "] Checking ICFG Node ID=" << icfgNode->getId();
        
        if (SVFUtil::isa<IntraICFGNode>(icfgNode)) {
            const std::string sourceLocation = icfgNode->getSourceLoc();
            SVFUtil::outs() << " (IntraICFGNode) at " << sourceLocation;
            
            llvm::json::Object locObj = GraphReaderUtil::parseSourceLocation(sourceLocation);
            if (!locObj.empty()) {
                if (auto cl = locObj.getInteger("cl")) {
                    SVFUtil::outs() << " [line=" << *cl << "]";
                    if (*cl == eqPosition) {
                        SVFUtil::outs() << " *** MATCHED ***";
                        matchedNodes.push_back(icfgNode);
                        if (!targetICFGNode) {
                            targetICFGNode = icfgNode;  // Keep first match
                        }
                    }
                }
            }
        } else {
            SVFUtil::outs() << " (Not IntraICFGNode, kind=" << icfgNode->getNodeKind() << ")";
        }
        SVFUtil::outs() << "\n";
    }
    
    // Report if multiple matches found
    if (matchedNodes.size() > 1) {
        SVFUtil::outs() << "  WARNING: Found " << matchedNodes.size() 
                        << " ICFG nodes matching eqPosition=" << eqPosition << ":\n";
        for (size_t i = 0; i < matchedNodes.size(); i++) {
            SVFUtil::outs() << "    [" << i << "] Node ID=" << matchedNodes[i]->getId() 
                            << " at " << matchedNodes[i]->getSourceLoc() << "\n";
        }
        SVFUtil::outs() << "  Using the first matched node (ID=" << targetICFGNode->getId() << ")\n";
    } else if (matchedNodes.size() == 1) {
        SVFUtil::outs() << "  Found exactly 1 matching ICFG node (ID=" << targetICFGNode->getId() << ")\n";
    }

    if (!targetICFGNode) {
        targetICFGNode = allICFGNodes[0];
        SVFUtil::outs() << "  Could not find exact match for eq position, using first ICFGNode\n";
    }

    SVFUtil::outs() << "  Found target ICFG Node ID=" << targetICFGNode->getId() << "\n";
    SVFUtil::outs() << "  ICFG Location: " << targetICFGNode->getSourceLoc() << "\n";

    // Find SVFG nodes at this ICFG location by reverse lookup
    // We need to traverse all SVFG nodes and find those whose getICFGNode() matches targetICFGNode
    const SVFGNode* startNode = nullptr;
    std::vector<const SVFGNode*> candidateNodes;

    SVFUtil::outs() << "  Searching SVFG nodes associated with ICFG " << targetICFGNode->getId() << "...\n";

    for (auto it = svfg->begin(); it != svfg->end(); ++it) {
        const SVFGNode* svfgNode = it->second;
        if (svfgNode->getICFGNode() == targetICFGNode) {
            candidateNodes.push_back(svfgNode);
            SVFUtil::outs() << "    Found SVFG Node " << svfgNode->getId() 
                            << " [" << GraphReaderUtil::getSVFGNodeKindString(svfgNode) << "]";
            // Verify the association
            const ICFGNode* verifyICFG = svfgNode->getICFGNode();
            if (verifyICFG) {
                SVFUtil::outs() << " -> ICFG " << verifyICFG->getId();
            }
            SVFUtil::outs() << "\n";
        }
    }

    if (candidateNodes.empty()) {
        GraphReaderUtil::sendJsonError("No SVFG nodes found at the target ICFG location");
        return;
    }

    // Prioritize certain node types that represent actual operations
    // Store, Copy, ActualParm (for calls like _TIFFmalloc)
    for (const SVFGNode* node : candidateNodes) {
        if (SVFUtil::isa<StoreSVFGNode>(node) || 
            SVFUtil::isa<CopySVFGNode>(node) ||
            SVFUtil::isa<ActualParmSVFGNode>(node)) {
            startNode = node;
            SVFUtil::outs() << "  Prioritizing node type: " << GraphReaderUtil::getSVFGNodeKindString(node) << "\n";
            break;
        }
    }

    // If no specific type found, use the first one
    if (!startNode) {
        startNode = candidateNodes[0];
    }

    SVFUtil::outs() << "  Selected start SVFG Node ID=" << startNode->getId() 
                    << " [" << GraphReaderUtil::getSVFGNodeKindString(startNode) << "]\n";
    
    // Verify the start node's ICFG association
    if (const ICFGNode* verifyICFG = startNode->getICFGNode()) {
        SVFUtil::outs() << "  Verification: Start SVFG -> ICFG " << verifyICFG->getId() 
                        << " at " << verifyICFG->getSourceLoc() << "\n";
    }

    // Step 2: Get the function and find actual return locations
    const FunObjVar* function = startNode->getFun();
    if (!function) {
        GraphReaderUtil::sendJsonError("Start node does not belong to a function");
        return;
    }

    SVFUtil::outs() << "[Step 2] Function: " << function->getName() << "\n";

    // Find actual return statement ICFG nodes (not wrappers)
    std::vector<const ICFGNode*> returnLocations = findActualReturnICFGNodes(icfg, function);
    SVFUtil::outs() << "  Found " << returnLocations.size() << " actual return location(s)\n";

    if (returnLocations.empty()) {
        SVFUtil::outs() << "  Function has no identifiable return locations\n";
        llvm::json::Object result;
        result["source_location"] = location;
        result["source_node_id"] = static_cast<int64_t>(startNode->getId());
        result["function"] = function->getName();
        result["return_locations"] = llvm::json::Array{};
        result["total_paths"] = 0;
        result["error"] = false;
        llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
        llvm::outs().flush();
        return;
    }

    // Step 3: For each return location, find all paths from startNode
    SVFUtil::outs() << "[Step 3] Searching for paths to each return location...\n";
    
    // First, let's check ICFG reachability from start node's ICFG
    const ICFGNode* startICFG = startNode->getICFGNode();
    SVFUtil::outs() << "  Start SVFG Node " << startNode->getId() << " -> ICFG Node " 
                    << (startICFG ? std::to_string(startICFG->getId()) : "NULL") << "\n";
    
    if (startICFG) {
        SVFUtil::outs() << "  Start ICFG Location: " << startICFG->getSourceLoc() << "\n";
        SVFUtil::outs() << "  Start ICFG has " << startICFG->getOutEdges().size() << " outgoing edges\n";
        
        // Show first few outgoing edges
        int edgeIdx = 0;
        for (const ICFGEdge* edge : startICFG->getOutEdges()) {
            if (edgeIdx < 3) {
                const ICFGNode* dst = edge->getDstNode();
                SVFUtil::outs() << "    Out edge " << edgeIdx << ": to ICFG " << dst->getId() 
                                << " at " << dst->getSourceLoc();
                if (SVFUtil::isa<CallCFGEdge>(edge)) {
                    SVFUtil::outs() << " [CallEdge]";
                } else if (SVFUtil::isa<RetCFGEdge>(edge)) {
                    SVFUtil::outs() << " [RetEdge]";
                } else if (SVFUtil::isa<IntraCFGEdge>(edge)) {
                    SVFUtil::outs() << " [IntraEdge]";
                }
                SVFUtil::outs() << "\n";
            }
            edgeIdx++;
        }
        if (edgeIdx > 3) {
            SVFUtil::outs() << "    ... and " << (edgeIdx - 3) << " more edges\n";
        }
        
        // Test ICFG-level reachability for each return location
        SVFUtil::outs() << "  [ICFG Reachability Test]\n";
        for (const ICFGNode* retLocation : returnLocations) {
            std::string retLoc = retLocation->getSourceLoc();
            SVFUtil::outs() << "    Testing reachability to " << retLoc << " (ICFG " << retLocation->getId() << ")...\n";
            bool reachable = canICFGReach(startICFG, retLocation, function, true);  // Enable debug
            SVFUtil::outs() << "    → " << retLoc << " : " << (reachable ? "REACHABLE" : "NOT REACHABLE") << "\n";
            
            if (reachable) {
                // Find ICFG paths to verify
                std::vector<std::vector<const ICFGNode*>> icfgPaths;
                findICFGPaths(startICFG, retLocation, function, icfgPaths);
                SVFUtil::outs() << "      ICFG paths found: " << icfgPaths.size() << "\n";
            }
        }
    }
    
    // Map: returnICFGNode -> list of paths
    std::map<const ICFGNode*, std::vector<std::vector<const SVFGNode*>>> pathsByReturn;

    for (const ICFGNode* retLocation : returnLocations) {
        std::string retLoc = retLocation->getSourceLoc();
        SVFUtil::outs() << "\n  [SVFG Path Search] Searching paths to return at: " << retLoc << "\n";
        SVFUtil::outs() << "    Target ICFG Node ID: " << retLocation->getId() << "\n";

        std::vector<std::vector<const SVFGNode*>> pathsToThisReturn;
        std::vector<const SVFGNode*> currentPath;
        currentPath.push_back(startNode);
        std::set<const SVFGNode*> visited;

        dfsToReturnLocation(startNode, retLocation, function, currentPath, visited, pathsToThisReturn);
        
        if (!pathsToThisReturn.empty()) {
            pathsByReturn[retLocation] = pathsToThisReturn;
            SVFUtil::outs() << "    ✓ Found " << pathsToThisReturn.size() << " SVFG path(s)\n";
        } else {
            SVFUtil::outs() << "    ✗ No SVFG paths found\n";
        }
    }

    // Step 4: Build JSON output grouped by return location
    llvm::json::Array returnLocationsArray;
    int globalPathId = 1;

    for (const auto& [retICFG, paths] : pathsByReturn) {
        llvm::json::Object retLocationObj;
        
        // Get location info for this return
        std::string retLocStr = retICFG->getSourceLoc();
        llvm::json::Object parsedLoc = GraphReaderUtil::parseSourceLocation(retLocStr);
        retLocationObj["location"] = std::move(parsedLoc);
        
        // Build paths array for this return location
        llvm::json::Array pathsArray;
        for (const auto& path : paths) {
            llvm::json::Object pathObj;
            pathObj["path_id"] = globalPathId++;
            
            // Build nodes array
            llvm::json::Array nodesArray;
            for (const SVFGNode* node : path) {
                llvm::json::Object nodeObj;
                nodeObj["node_id"] = static_cast<int64_t>(node->getId());
                nodeObj["node_type"] = GraphReaderUtil::getSVFGNodeKindString(node);
                nodeObj["node_desc"] = node->toString();
                nodesArray.push_back(std::move(nodeObj));
            }
            
            pathObj["nodes"] = std::move(nodesArray);
            pathsArray.push_back(std::move(pathObj));
        }
        
        retLocationObj["paths"] = std::move(pathsArray);
        retLocationObj["path_count"] = static_cast<int64_t>(paths.size());
        returnLocationsArray.push_back(std::move(retLocationObj));
    }

    int totalPaths = globalPathId - 1;
    SVFUtil::outs() << "  Total paths found across all returns: " << totalPaths << "\n";

    llvm::json::Object result;
    result["source_location"] = location;
    result["source_node_id"] = static_cast<int64_t>(startNode->getId());
    result["function"] = function->getName();
    result["return_locations"] = std::move(returnLocationsArray);
    result["total_return_locations"] = static_cast<int64_t>(pathsByReturn.size());
    result["total_paths"] = static_cast<int64_t>(totalPaths);
    result["error"] = false;

    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();

    SVFUtil::outs() << "\n========================================\n";
    SVFUtil::outs() << "[findPathsToFormalOUT] Complete\n";
    SVFUtil::outs() << "========================================\n\n";
}

void PathQuery::getConditionReturnInsidePath(const std::string& startLocation) {
    // DEBUG
    SVFUtil::outs() << "\n========================================\n";
    SVFUtil::outs() << "[getConditionReturnInsidePath] Start Location: " << startLocation << "\n";
    SVFUtil::outs() << "========================================\n\n";
    
    if (!icfg) {
        GraphReaderUtil::sendJsonError("ICFG is null!");
        return;
    }

    // Step 1: Find the starting ICFG node
    const ICFGNode* startNode = GraphReaderUtil::findICFGNodeByLocation(icfg, startLocation);
    if (!startNode) {
        GraphReaderUtil::sendJsonError("Cannot find ICFGNode for location: " + startLocation);
        return;
    }
    
    const FunObjVar* function = startNode->getFun();
    if (!function) {
        GraphReaderUtil::sendJsonError("Start location is not inside any function");
        return;
    }
    
    SVFUtil::outs() << "[Step 1] Found start ICFG Node ID=" << startNode->getId() << "\n";
    SVFUtil::outs() << "  Function: " << function->getName() << "\n";
    SVFUtil::outs() << "  Location: " << startNode->getSourceLoc() << "\n";

    // Step 2: Find all actual return locations in the function
    std::vector<const ICFGNode*> returnLocations = findActualReturnICFGNodes(icfg, function);
    SVFUtil::outs() << "\n[Step 2] Found " << returnLocations.size() << " actual return location(s)\n";
    
    for (const ICFGNode* retLoc : returnLocations) {
        std::string retSourceLoc = retLoc->getSourceLoc();
        if (retSourceLoc.empty()) {
            SVFUtil::outs() << "  - <no source location> (ICFG ID: " << retLoc->getId() << ")\n";
        } else {
            SVFUtil::outs() << "  - " << retSourceLoc << " (ICFG ID: " << retLoc->getId() << ")\n";
        }

        // Provide additional debug context for each return node
        std::string nodeType = "Unknown";
        if (SVFUtil::isa<IntraICFGNode>(retLoc)) {
            nodeType = "IntraICFGNode";
        } else if (SVFUtil::isa<CallICFGNode>(retLoc)) {
            nodeType = "CallICFGNode";
        } else if (SVFUtil::isa<RetICFGNode>(retLoc)) {
            nodeType = "RetICFGNode";
        } else if (SVFUtil::isa<FunExitICFGNode>(retLoc)) {
            nodeType = "FunExitICFGNode";
        } else if (SVFUtil::isa<FunEntryICFGNode>(retLoc)) {
            nodeType = "FunEntryICFGNode";
        } else if (SVFUtil::isa<GlobalICFGNode>(retLoc)) {
            nodeType = "GlobalICFGNode";
        }
        SVFUtil::outs() << "      Node Type: " << nodeType << "\n";

        llvm::json::Object parsedLoc = GraphReaderUtil::parseSourceLocation(retSourceLoc);
        if (!parsedLoc.empty()) {
            if (auto file = parsedLoc.getString("fl")) {
                SVFUtil::outs() << "      File: " << file->str() << "\n";
            }
            if (auto line = parsedLoc.getInteger("ln")) {
                SVFUtil::outs() << "      Line: " << *line << "\n";
            }
            if (auto col = parsedLoc.getInteger("cl")) {
                SVFUtil::outs() << "      Column: " << *col << "\n";
            }
        } else {
            SVFUtil::outs() << "      Parsed location is empty. Attempting to recover via LLVM debug info...\n";
        }

        if (const IntraICFGNode* intraNode = SVFUtil::dyn_cast<IntraICFGNode>(retLoc)) {
            LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
            const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(intraNode);
            if (const llvm::Instruction* inst = llvm::dyn_cast_or_null<llvm::Instruction>(llvmVal)) {
                SVFUtil::outs() << "      LLVM Opcode: " << inst->getOpcodeName() << "\n";
                const llvm::DebugLoc& debugLoc = inst->getDebugLoc();
                if (debugLoc) {
                    const llvm::DILocation* diLoc = debugLoc.get();
                    SVFUtil::outs() << "      DebugLoc: "
                                     << (diLoc ? diLoc->getFilename().str() : std::string("<unknown>"))
                                     << ":" << debugLoc.getLine()
                                     << ":" << debugLoc.getCol() << "\n";
                } else {
                    SVFUtil::outs() << "      DebugLoc: <none>\n";
                }
            } else {
                SVFUtil::outs() << "      LLVM Instruction: <none>\n";
            }
        }

        SVFUtil::outs() << "      Outgoing Edges: " << retLoc->getOutEdges().size() << "\n";
        SVFUtil::outs() << "      Incoming Edges: " << retLoc->getInEdges().size() << "\n";
    }

    if (returnLocations.empty()) {
        llvm::json::Object result;
        result["start_location"] = startLocation;
        result["function"] = function->getName();
        result["return_locations"] = llvm::json::Array{};
        result["total_paths"] = 0;
        result["error"] = false;
        llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
        llvm::outs().flush();
        return;
    }

    // Step 3: For each return location, find all ICFG paths
    SVFUtil::outs() << "\n[Step 3] Searching for ICFG paths to each return location...\n";
    
    std::map<const ICFGNode*, std::vector<std::vector<const ICFGNode*>>> pathsByReturn;

    for (const ICFGNode* retLocation : returnLocations) {
        SVFUtil::outs() << "  Searching paths to: " << retLocation->getSourceLoc() << "\n";
        
        std::vector<std::vector<const ICFGNode*>> pathsToThisReturn;
        findICFGPaths(startNode, retLocation, function, pathsToThisReturn);
        
        if (!pathsToThisReturn.empty()) {
            pathsByReturn[retLocation] = pathsToThisReturn;
            SVFUtil::outs() << "    Found " << pathsToThisReturn.size() << " path(s)\n";
        } else {
            SVFUtil::outs() << "    No paths found\n";
        }
    }

    // Step 4: Build JSON output - collect SVFG nodes along ICFG paths
    SVFUtil::outs() << "\n[Step 4] Collecting SVFG nodes along ICFG paths...\n";
    llvm::json::Array returnLocationsArray;
    int globalPathId = 1;

    // Structure to store collected node information for each path
    struct PathNodeInfo {
        Set<NodeID> nodeIds;  // Node IDs in this path
        std::vector<const SVFGNode*> nodes;  // Ordered list of nodes
    };

    for (const auto& [retICFG, icfgPaths] : pathsByReturn) {
        llvm::json::Object retLocationObj;
        
        // Get location info for this return
        std::string retLocStr = retICFG->getSourceLoc();
        llvm::json::Object parsedLoc = GraphReaderUtil::parseSourceLocation(retLocStr);
        retLocationObj["location"] = std::move(parsedLoc);
        
        // Step 4a: First pass - collect all SVFG nodes for all paths to this return location
        std::vector<PathNodeInfo> pathNodeInfos;
        pathNodeInfos.reserve(icfgPaths.size());
        
        for (const auto& icfgPath : icfgPaths) {
            PathNodeInfo pathInfo;
            
            SVFUtil::outs() << "  Path " << (globalPathId + pathNodeInfos.size()) << ": Collecting SVFG nodes...\n";
            
            for (const ICFGNode* icfgNode : icfgPath) {
                // Find all SVFG nodes at this ICFG location
                for (auto it = svfg->begin(); it != svfg->end(); ++it) {
                    const SVFGNode* svfgNode = it->second;
                    if (svfgNode->getICFGNode() == icfgNode) {
                        // Only record Branch and MIntraPhi nodes
                        bool isKeyNode = false;
                        
                        if (SVFUtil::isa<IntraMSSAPHISVFGNode>(svfgNode)) {
                            isKeyNode = true;  // Memory state merge point (MIntraPhi)
                        } 
                        // else if (SVFUtil::isa<BranchVFGNode>(svfgNode)) {
                        //     isKeyNode = true;  // Control flow branch
                        // }
                        
                        if (isKeyNode && !pathInfo.nodeIds.count(svfgNode->getId())) {
                            pathInfo.nodeIds.insert(svfgNode->getId());
                            pathInfo.nodes.push_back(svfgNode);
                            
                            SVFUtil::outs() << "    + SVFG Node " << svfgNode->getId() 
                                            << " [" << GraphReaderUtil::getSVFGNodeKindString(svfgNode) << "]\n";
                        }
                    }
                }
            }
            
            pathNodeInfos.push_back(std::move(pathInfo));
        }
        
        // Step 4b: Compute differential nodes (key events) for multiple paths
        bool hasMultiplePaths = icfgPaths.size() > 1;
        std::vector<Set<NodeID>> differentialNodes;
        Set<NodeID> allDifferentialNodes;  // Union of all differential nodes
        std::map<NodeID, const SVFGNode*> nodeIdToSVFGNode;  // Map for quick lookup
        
        if (hasMultiplePaths) {
            SVFUtil::outs() << "  Computing differential nodes for " << icfgPaths.size() << " paths...\n";
            
            // Build node ID to SVFG node mapping
            for (const auto& pathInfo : pathNodeInfos) {
                for (const SVFGNode* node : pathInfo.nodes) {
                    nodeIdToSVFGNode[node->getId()] = node;
                }
            }
            
            // Compute differential nodes for each path
            for (size_t i = 0; i < pathNodeInfos.size(); i++) {
                Set<NodeID> differential;
                
                // For each node in this path, check if it appears in all other paths
                for (NodeID nodeId : pathNodeInfos[i].nodeIds) {
                    bool uniqueToThisPath = false;
                    
                    // Check if this node is missing from any other path
                    for (size_t j = 0; j < pathNodeInfos.size(); j++) {
                        if (i != j && !pathNodeInfos[j].nodeIds.count(nodeId)) {
                            uniqueToThisPath = true;
                            break;
                        }
                    }
                    
                    if (uniqueToThisPath) {
                        differential.insert(nodeId);
                        allDifferentialNodes.insert(nodeId);
                    }
                }
                
                differentialNodes.push_back(differential);
                SVFUtil::outs() << "    Path " << (globalPathId + i) << " has " 
                                << differential.size() << " differential node(s)\n";
            }
            
            SVFUtil::outs() << "  Total unique differential nodes across all paths: " 
                            << allDifferentialNodes.size() << "\n";
        }
        
        // Step 4c: Build JSON output
        llvm::json::Array pathsArray;
        for (size_t i = 0; i < pathNodeInfos.size(); i++) {
            llvm::json::Object pathObj;
            pathObj["path_id"] = globalPathId++;
            
            const PathNodeInfo& pathInfo = pathNodeInfos[i];
            
            // Build key_events array
            llvm::json::Array keyEventsArray;
            
            if (hasMultiplePaths) {
                // For multiple paths: include all differential nodes with pass field
                for (NodeID nodeId : allDifferentialNodes) {
                    llvm::json::Object keyEventObj;
                    
                    const SVFGNode* svfgNode = nodeIdToSVFGNode[nodeId];
                    keyEventObj["node_id"] = static_cast<int64_t>(nodeId);
                    keyEventObj["node_type"] = GraphReaderUtil::getSVFGNodeKindString(svfgNode);
                    // keyEventObj["node_desc"] = svfgNode->toString();
                    
                    // pass: true if this node exists in current path, false otherwise
                    keyEventObj["pass"] = pathInfo.nodeIds.count(nodeId) > 0;
                    
                    keyEventsArray.push_back(std::move(keyEventObj));
                }
            }
            // For single path: empty key_events array
            
            pathObj["key_events"] = std::move(keyEventsArray);
            pathsArray.push_back(std::move(pathObj));
        }
        
        retLocationObj["paths"] = std::move(pathsArray);
        retLocationObj["path_count"] = static_cast<int64_t>(icfgPaths.size());
        returnLocationsArray.push_back(std::move(retLocationObj));
    }

    int totalPaths = globalPathId - 1;
    SVFUtil::outs() << "\n[Step 4] Total paths found: " << totalPaths << "\n";

    llvm::json::Object result;
    result["start_location"] = startLocation;
    result["function"] = function->getName();
    result["return_locations"] = std::move(returnLocationsArray);
    result["total_return_locations"] = static_cast<int64_t>(pathsByReturn.size());
    result["total_paths"] = static_cast<int64_t>(totalPaths);
    result["error"] = false;

    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();

    SVFUtil::outs() << "\n========================================\n";
    SVFUtil::outs() << "[getConditionReturnInsidePath] Complete\n";
    SVFUtil::outs() << "========================================\n\n";
}

void PathQuery::getValueSensitiveReturnInsidePathImpl(
    const std::string& startLocation,
    const std::vector<const SVFGNode*>& startSVFGNodes,
    const PAGNode* targetPAG) {
    if (!icfg || !svfg || !pag) {
        GraphReaderUtil::sendJsonError("ICFG, SVFG, or PAG is null for value-sensitive analysis!");
        return;
    }

    if (startSVFGNodes.empty()) {
        GraphReaderUtil::sendJsonError("No SVFG start nodes provided for value-sensitive analysis.");
        return;
    }

    const ICFGNode* startNode = GraphReaderUtil::findICFGNodeByLocation(icfg, startLocation);
    if (!startNode) {
        GraphReaderUtil::sendJsonError("Cannot find ICFGNode for location: " + startLocation);
        return;
    }

    const FunObjVar* function = startNode->getFun();
    if (!function) {
        GraphReaderUtil::sendJsonError("Start location is not inside any function");
        return;
    }

    // Step C: Find all reachable SVFGNodes from provided start nodes (keySVFGNodes)
    Set<const SVFGNode*> keySVFGNodes;
    std::queue<const SVFGNode*> worklist;
    std::vector<const SVFGNode*> validStartNodes;

    auto enqueueStartNode = [&](const SVFGNode* node) {
        if (!node) {
            return;
        }
        if (node->getFun() != function) {
            SVFUtil::errs() << "[getValueSensitiveReturnInsidePath] Ignoring start SVFGNode "
                            << node->getId() << " because it belongs to function '"
                            << (node->getFun() ? node->getFun()->getName() : "unknown")
                            << "' instead of '" << function->getName() << "'.\n";
            return;
        }
        if (keySVFGNodes.insert(node).second) {
            worklist.push(node);
            validStartNodes.push_back(node);
        }
    };

    for (const SVFGNode* startNodeCandidate : startSVFGNodes) {
        enqueueStartNode(startNodeCandidate);
    }

    if (validStartNodes.empty()) {
        GraphReaderUtil::sendJsonError("No valid SVFG start nodes belong to function '" + function->getName() + "'.");
        return;
    }

    while (!worklist.empty()) {
        const SVFGNode* currentNode = worklist.front();
        worklist.pop();

        for (const SVFGEdge* edge : currentNode->getOutEdges()) {
            const SVFGNode* nextNode = edge->getDstNode();
            if (nextNode->getFun() != function) {
                continue;
            }
            if (keySVFGNodes.insert(nextNode).second) {
                worklist.push(nextNode);
            }
        }
    }

    // Step D: Find all ICFG paths to return locations
    std::vector<const ICFGNode*> returnLocations = findActualReturnICFGNodes(icfg, function);

    if (returnLocations.empty()) {
        llvm::json::Object result;
        result["start_location"] = startLocation;
        result["function"] = function->getName();
        result["return_locations"] = llvm::json::Array{};
        result["total_paths"] = 0;
        result["error"] = false;
        result["key_svfg_nodes_count"] = static_cast<int64_t>(keySVFGNodes.size());
        llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
        llvm::outs().flush();
        return;
    }

    // Find all ICFG paths to each return location
    std::map<const ICFGNode*, std::vector<std::vector<const ICFGNode*>>> pathsByReturn;

    for (const ICFGNode* retLocation : returnLocations) {
        std::vector<std::vector<const ICFGNode*>> pathsToThisReturn;
        findICFGPaths(startNode, retLocation, function, pathsToThisReturn);
        
        if (!pathsToThisReturn.empty()) {
            pathsByReturn[retLocation] = pathsToThisReturn;
        }
    }

    // Step E & F: For each path, collect keySVFGNode sequence and group by sequence
    // Structure: return location -> (keySVFGNode sequence -> list of path indices)
    std::map<const ICFGNode*, std::map<std::vector<NodeID>, std::vector<int>>> pathGroupsByReturn;
    
    // Also store the actual ICFG paths for later reference
    std::map<const ICFGNode*, std::vector<std::vector<const ICFGNode*>>> allICFGPathsByReturn = pathsByReturn;

    for (const auto& [retICFG, icfgPaths] : pathsByReturn) {
        for (size_t pathIdx = 0; pathIdx < icfgPaths.size(); pathIdx++) {
            const auto& icfgPath = icfgPaths[pathIdx];
            
            // Collect keySVFGNode sequence for this path
            std::vector<NodeID> keySVFGSequence;
            Set<NodeID> seenInPath;  // Avoid duplicates in same path
            
            for (const ICFGNode* icfgNode : icfgPath) {
                // Find all SVFG nodes at this ICFG location
                for (auto it = svfg->begin(); it != svfg->end(); ++it) {
                    const SVFGNode* svfgNode = it->second;
                    if (svfgNode->getICFGNode() == icfgNode) {
                        // Check if this is a keySVFGNode
                        if (keySVFGNodes.count(svfgNode) && !seenInPath.count(svfgNode->getId())) {
                            // OUTPUT FILTER: Hide certain node types from the final sequence
                            // All nodes were collected during BFS, now we filter what to display
                            bool shouldHideInOutput = false;
                            
                            // Filter out pure control-flow nodes (don't affect data/memory)
                            if (SVFUtil::isa<BranchVFGNode>(svfgNode)) {
                                shouldHideInOutput = true;
                            } else if (SVFUtil::isa<NullPtrSVFGNode>(svfgNode)) {
                                shouldHideInOutput = true;
                            } else if (SVFUtil::isa<DummyVersionPropSVFGNode>(svfgNode)) {
                                shouldHideInOutput = true;
                            } else if (SVFUtil::isa<BinaryOPVFGNode>(svfgNode)) {
                                shouldHideInOutput = true;
                            } else if (SVFUtil::isa<UnaryOPVFGNode>(svfgNode)) {
                                shouldHideInOutput = true;
                            } else if (SVFUtil::isa<CmpVFGNode>(svfgNode)) {
                                shouldHideInOutput = true;
                            }
                            
                            // Optionally filter out IntraMSSAPHI and Load nodes from output
                            // Uncomment these lines if you want to hide them:
                            else if (SVFUtil::isa<IntraMSSAPHISVFGNode>(svfgNode)) {
                                shouldHideInOutput = true;
                            } else if (SVFUtil::isa<LoadVFGNode>(svfgNode)) {
                                shouldHideInOutput = true;
                            }
                            
                            if (!shouldHideInOutput) {
                                keySVFGSequence.push_back(svfgNode->getId());
                                seenInPath.insert(svfgNode->getId());
                            }
                        }
                    }
                }
            }
            
            // Group this path by its keySVFGNode sequence
            pathGroupsByReturn[retICFG][keySVFGSequence].push_back(pathIdx);
        }
    }

    // Step G: Build JSON output with differential analysis
    llvm::json::Array returnLocationsArray;
    int groupIdCounter = 1;
    int totalPaths = 0;

    for (const auto& [retICFG, pathGroups] : pathGroupsByReturn) {
        llvm::json::Object retLocationObj;
        
        // Get location info for this return
        std::string retLocStr = retICFG->getSourceLoc();
        llvm::json::Object parsedLoc = GraphReaderUtil::parseSourceLocation(retLocStr);
        retLocationObj["location"] = std::move(parsedLoc);
        
        // Differential analysis: compute common nodes (intersection) and union
        Set<NodeID> commonNodes;
        Set<NodeID> unionNodes;
        bool isFirst = true;
        
        for (const auto& [keySVFGSequence, pathIndices] : pathGroups) {
            Set<NodeID> currentSet(keySVFGSequence.begin(), keySVFGSequence.end());
            
            if (isFirst) {
                commonNodes = currentSet;
                isFirst = false;
            } else {
                // Intersection: keep only nodes that appear in all groups
                Set<NodeID> newCommon;
                for (NodeID nodeId : commonNodes) {
                    if (currentSet.count(nodeId)) {
                        newCommon.insert(nodeId);
                    }
                }
                commonNodes = newCommon;
            }
            
            // Union: add all nodes
            for (NodeID nodeId : currentSet) {
                unionNodes.insert(nodeId);
            }
        }
        
        // Build path groups array with differential info
        llvm::json::Array pathGroupsArray;
        
        for (const auto& [keySVFGSequence, pathIndices] : pathGroups) {
            llvm::json::Object groupObj;
            groupObj["group_id"] = groupIdCounter++;
            
            Set<NodeID> currentSet(keySVFGSequence.begin(), keySVFGSequence.end());
            
            // key_svfg_sequence: nodes in current group but not in common nodes (extra nodes)
            llvm::json::Array sequenceArray;
            llvm::json::Array sequenceDescArray;
            
            for (NodeID nodeId : keySVFGSequence) {
                if (!commonNodes.count(nodeId)) {
                    sequenceArray.push_back(static_cast<int64_t>(nodeId));
                    
                    // Get node description using toString()
                    const SVFGNode* node = svfg->getSVFGNode(nodeId);
                    if (node) {
                        sequenceDescArray.push_back(node->toString());
                    } else {
                        sequenceDescArray.push_back("unknown");
                    }
                }
            }
            
            groupObj["key_svfg_sequence"] = std::move(sequenceArray);
            groupObj["key_svfg_sequence_desc"] = std::move(sequenceDescArray);
            
            // Count paths for statistics (but don't output to JSON)
            totalPaths += pathIndices.size();
            
            pathGroupsArray.push_back(std::move(groupObj));
        }
        
        retLocationObj["path_groups"] = std::move(pathGroupsArray);
        retLocationObj["mergeable_groups"] = static_cast<int64_t>(pathGroups.size());
        
        // Add common nodes info to return location
        llvm::json::Array commonNodesArray;
        for (NodeID nodeId : commonNodes) {
            commonNodesArray.push_back(static_cast<int64_t>(nodeId));
        }
        retLocationObj["common_nodes"] = std::move(commonNodesArray);
        retLocationObj["common_nodes_count"] = static_cast<int64_t>(commonNodes.size());
        
        returnLocationsArray.push_back(std::move(retLocationObj));
    }

    // Build final result
    llvm::json::Object result;
    result["start_location"] = startLocation;
    result["function"] = function->getName();
    result["key_svfg_nodes_count"] = static_cast<int64_t>(keySVFGNodes.size());
    result["return_locations"] = std::move(returnLocationsArray);
    result["total_return_locations"] = static_cast<int64_t>(pathGroupsByReturn.size());
    result["total_paths"] = static_cast<int64_t>(totalPaths);
    result["total_path_groups"] = static_cast<int64_t>(groupIdCounter - 1);
    result["error"] = false;

    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}

void PathQuery::getValueSensitiveReturnInsidePath(const std::string& startLocation, const SVFGNode* startSVFGNode) {
    std::vector<const SVFGNode*> startNodes;
    if (startSVFGNode) {
        startNodes.push_back(startSVFGNode);
    }
    getValueSensitiveReturnInsidePathImpl(startLocation, startNodes, nullptr);
}

void PathQuery::getValueSensitiveReturnInsidePath(const std::string& startLocation,
                                                  const std::vector<const SVFGNode*>& startSVFGNodes) {
    getValueSensitiveReturnInsidePathImpl(startLocation, startSVFGNodes, nullptr);
}

void PathQuery::getValueSensitiveReturnInsidePath(const std::string& startLocation, const PAGNode* targetPAG) {
    if (!targetPAG) {
        GraphReaderUtil::sendJsonError("Target PAG node is null for value-sensitive analysis.");
        return;
    }

    if (!svfg) {
        GraphReaderUtil::sendJsonError("SVFG is null; cannot resolve start SVFG nodes from PAG node.");
        return;
    }

    std::vector<const SVFGNode*> startNodes;
    if (svfg->hasDefSVFGNode(targetPAG)) {
        startNodes.push_back(svfg->getDefSVFGNode(targetPAG));
    }

    if (startNodes.empty()) {
        GraphReaderUtil::sendJsonError("Cannot find any SVFG definition node for PAG node " + std::to_string(targetPAG->getId()));
        return;
    }

    getValueSensitiveReturnInsidePathImpl(startLocation, startNodes, targetPAG);
}

void PathQuery::traceCallArgToReturn(const std::string& callLocation,
                                     const std::string& functionName,
                                     int argIndex) {
    SVFUtil::outs() << "\n========================================\n";
    SVFUtil::outs() << "[traceCallArgToReturn] Location: " << callLocation
                    << ", Function: " << functionName
                    << ", Arg Index: " << argIndex << "\n";
    SVFUtil::outs() << "========================================\n";

    // Step 1: Use traceCallArgumentValueFlow to find the definition point
    const PAGNode* defPAGNode = GraphReaderUtil::traceCallArgumentValueFlow(
        svfg, icfg, pag, callLocation, functionName, argIndex);
    
    if (!defPAGNode) {
        GraphReaderUtil::sendJsonError("Could not find definition PAGNode for call argument");
        SVFUtil::errs() << "[traceCallArgToReturn] Failed to resolve definition PAG node."
                        << " Location: " << callLocation
                        << ", Function: " << functionName
                        << ", Arg Index: " << argIndex << "\n";
        return;
    }

    SVFUtil::outs() << "[traceCallArgToReturn] Resolved definition PAGNode ID: "
                    << defPAGNode->getId() << ", Desc: " << defPAGNode->toString() << "\n";

    // Step 2: Call getValueSensitiveReturnInsidePath with the definition PAGNode
    // This will trace all paths from callLocation to function returns,
    // performing value-sensitive analysis based on defPAGNode
    getValueSensitiveReturnInsidePath(callLocation, defPAGNode);

    SVFUtil::outs() << "[traceCallArgToReturn] Completed value-sensitive tracing for call at "
                    << callLocation << "\n";
}

}// namespace SVF
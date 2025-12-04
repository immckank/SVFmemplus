#include "PathQuery.h"
#include "GraphReaderUtil.h"
#include "Graphs/ICFG.h"
#include "Graphs/SVFGEdge.h"
#include "Graphs/CallGraph.h"
#include "SVFIR/SVFValue.h"
#include "Graphs/VFGNode.h"
#include "Graphs/SVFG.h"
#include "Util/SVFUtil.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/SVFVariables.h"
#include "SVFIR/SVFType.h"
#include "MemoryModel/PointerAnalysis.h"
#include "MemoryModel/PointsTo.h"
#include "SABER/SaberCondAllocator.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <deque>
#include <iostream>
#include <map>
#include <queue>
#include <set>

namespace SVF {

void PathQuery::getValueInsidePath(const SVFGNode* startNode) {
    // DEBUG
    // 一个可能有用的debug功能 暂时留着
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

void PathQuery::getConditionPath(const std::string& startLocation, const std::string& targetLocation) {
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

void PathQuery::getConditionInsidePath(const std::string& startLocation, const std::string& targetLocation) {
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

void PathQuery::getConstrainInside(const std::string& location) {
    SVFUtil::outs() << "[getConstrainInside] Start location: " << location << "\n";

    if (!icfg) {
        GraphReaderUtil::sendJsonError("ICFG is null!");
        return;
    }

    std::vector<const ICFGNode*> icfgNodes = GraphReaderUtil::findAllICFGNodesByLocation(icfg, location);
    if (icfgNodes.empty()) {
        GraphReaderUtil::sendJsonError("Cannot find ICFG nodes for location: " + location);
        return;
    }

    const ICFGNode* targetNode = nullptr;
    const FunObjVar* function = nullptr;
    FunEntryICFGNode* entryNode = nullptr;

    for (const ICFGNode* node : icfgNodes) {
        if (!node) {
            continue;
        }
        const FunObjVar* fun = node->getFun();
        if (!fun) {
            continue;
        }

        FunEntryICFGNode* candidateEntry = icfg->getFunEntryICFGNode(const_cast<FunObjVar*>(fun));
        if (!candidateEntry) {
            continue;
        }

        targetNode = node;
        function = fun;
        entryNode = candidateEntry;
        break;
    }

    if (!targetNode || !function || !entryNode) {
        GraphReaderUtil::sendJsonError("Failed to resolve function entry for location: " + location);
        return;
    }

    SVFUtil::outs() << "[getConstrainInside] Target ICFG node id: " << targetNode->getId()
                    << " function: " << function->getName() << "\n";
    SVFUtil::outs() << "[getConstrainInside] Entry ICFG node id: " << entryNode->getId() << "\n";

    std::vector<std::vector<const ICFGNode*>> icfgPaths;
    findICFGPaths(entryNode, targetNode, function, icfgPaths);

    llvm::json::Object result;
    result["error"] = false;
    result["function"] = function->getName();
    result["path_count"] = static_cast<int64_t>(icfgPaths.size());
    result["target_icfg_node_id"] = static_cast<int64_t>(targetNode->getId());

    llvm::json::Object targetLocInfo = GraphReaderUtil::parseSourceLocation(targetNode->getSourceLoc());
    if (targetLocInfo.empty()) {
        targetLocInfo = GraphReaderUtil::parseSourceLocation(location);
    }
    result["target_location"] = std::move(targetLocInfo);

    if (icfgPaths.empty()) {
        result["message"] = "No intra-procedural paths found from function entry to target location.";
        llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
        llvm::outs().flush();
        return;
    }

    llvm::json::Array pathsArray;
    std::map<std::string, llvm::json::Object> keyToEvent;
    std::set<std::string> commonEventKeys;
    bool firstPath = true;
    int pathId = 1;

    for (const auto& path : icfgPaths) {
        llvm::json::Object pathObj;
        pathObj["path_id"] = pathId++;
        pathObj["node_count"] = static_cast<int64_t>(path.size());

        llvm::json::Array constraintEvents;
        std::set<std::string> pathEventKeys;

        for (size_t idx = 0; idx + 1 < path.size(); ++idx) {
            const ICFGNode* srcNode = path[idx];
            const ICFGNode* dstNode = path[idx + 1];
            const IntraCFGEdge* branchEdge = nullptr;

            for (ICFGEdge* edge : srcNode->getOutEdges()) {
                if (edge->getDstNode() != dstNode) {
                    continue;
                }
                if (const IntraCFGEdge* intraEdge = SVFUtil::dyn_cast<IntraCFGEdge>(edge)) {
                    if (intraEdge->getCondition()) {
                        branchEdge = intraEdge;
                        break;
                    }
                }
            }

            if (!branchEdge) {
                continue;
            }

            llvm::json::Object branchInfo = GraphReaderUtil::formatBranchInfo(branchEdge);
            branchInfo["src_icfg_node_id"] = static_cast<int64_t>(srcNode->getId());
            branchInfo["dst_icfg_node_id"] = static_cast<int64_t>(dstNode->getId());

            std::string locKey = "unknown";
            if (auto loc = branchInfo.getString("location")) {
                locKey = loc->str();
            }
            std::string condKey = "unknown";
            if (auto cond = branchInfo.getString("condition_value")) {
                condKey = cond->str();
            }
            std::string eventKey = locKey + "|" + condKey;

            keyToEvent[eventKey] = branchInfo;
            pathEventKeys.insert(eventKey);

            constraintEvents.push_back(std::move(branchInfo));
        }

        if (firstPath) {
            commonEventKeys = pathEventKeys;
            firstPath = false;
        } else {
            std::set<std::string> intersection;
            for (const std::string& key : commonEventKeys) {
                if (pathEventKeys.count(key)) {
                    intersection.insert(key);
                }
            }
            commonEventKeys = std::move(intersection);
        }

        pathObj["constraints"] = std::move(constraintEvents);
        pathsArray.push_back(std::move(pathObj));
    }

    result["paths"] = std::move(pathsArray);

    llvm::json::Array commonConstraints;
    for (const std::string& key : commonEventKeys) {
        auto it = keyToEvent.find(key);
        if (it != keyToEvent.end()) {
            llvm::json::Object copyObj = it->second;
            commonConstraints.push_back(std::move(copyObj));
        }
    }
    result["common_constraints"] = std::move(commonConstraints);
    result["common_constraint_count"] = static_cast<int64_t>(commonEventKeys.size());

    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
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

// Helper function to find all ICFG paths from start to target (intra-procedural)
void PathQuery::findICFGPaths(const ICFGNode* startICFG, const ICFGNode* targetICFG, const FunObjVar* function, std::vector<std::vector<const ICFGNode*>>& allICFGPaths) {
    // TOOL FUNCTION
    // Use BFS to find paths (prioritizes shorter paths over DFS) while allowing limited node revisits
    struct PathState {
        const ICFGNode* node;
        std::vector<const ICFGNode*> path;
        llvm::DenseMap<const ICFGNode*, unsigned> visitCounts;
    };

    std::deque<PathState> worklist;
    PathState initial;
    initial.node = startICFG;
    initial.path.push_back(startICFG);
    initial.visitCounts[startICFG] = 1;
    worklist.push_back(std::move(initial));
    
    // Add limits to prevent explosion but bias toward quickly finding reachable paths
    const size_t MAX_PATHS = 10;              // Focus on a handful of representative paths
    const size_t MAX_PATH_LENGTH = 120;       // Slightly relaxed to allow loop exits
    const size_t MAX_ITERATIONS = 20000;      // Cap exploration time
    const unsigned MAX_NODE_VISITS = 2;       // Allow loops to be re-entered once
    
    size_t iterations = 0;
    
    while (!worklist.empty()) {
        iterations++;
        
        if (iterations > MAX_ITERATIONS || allICFGPaths.size() >= MAX_PATHS) {
            break;
        }
        
        PathState current = std::move(worklist.front());
        worklist.pop_front();
        
        if (current.path.size() > MAX_PATH_LENGTH) {
            continue;
        }
        
        if (current.node == targetICFG) {
            allICFGPaths.push_back(current.path);
            continue;
        }
        
        for (ICFGEdge* edge : current.node->getOutEdges()) {
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
            
            if (!succNode) {
                continue;
            }
            
            // Only traverse within the same function
            if (succNode->getFun() != function) {
                continue;
            }
            
            unsigned visits = current.visitCounts.lookup(succNode);
            if (visits >= MAX_NODE_VISITS) {
                continue;
            }
            
            PathState nextState;
            nextState.node = succNode;
            nextState.path = current.path;
            nextState.path.push_back(succNode);
            nextState.visitCounts = current.visitCounts;
            nextState.visitCounts[succNode] = visits + 1;
            
            worklist.push_back(std::move(nextState));
        }
    }
}

/*!
 * Helper function to collect Z3 conditions along an ICFG path.
 * Returns the combined path condition (AND of all branch conditions).
 * Returns true condition if no conditions are found or condAllocator is null.
 */
static SaberCondAllocator::Condition collectPathCondition(
    const std::vector<const ICFGNode*>& path,
    SaberCondAllocator* condAllocator) {
    
    if (!condAllocator || path.size() < 2) {
        // If no allocator or path too short, return true condition (feasible)
        return condAllocator ? condAllocator->getTrueCond() : SaberCondAllocator::Condition::getTrueCond();
    }
    
    SaberCondAllocator::Condition pathCond = condAllocator->getTrueCond();
    bool hasCondition = false;
    
    // Iterate through consecutive pairs of nodes in the path
    for (size_t i = 0; i < path.size() - 1; ++i) {
        const ICFGNode* srcNode = path[i];
        const ICFGNode* dstNode = path[i + 1];
        
        // Get basic blocks - skip if nodes don't have basic blocks (e.g., call/return nodes)
        const SVFBasicBlock* srcBB = srcNode->getBB();
        const SVFBasicBlock* dstBB = dstNode->getBB();
        
        if (!srcBB || !dstBB) {
            // Skip nodes without basic blocks (call/return nodes, etc.)
            continue;
        }
        
        
        // Only process if both nodes are in the same basic block or there's an edge between them
        // Check if there's an edge from srcNode to dstNode
        ICFGEdge* edge = nullptr;
        for (ICFGEdge* outEdge : srcNode->getOutEdges()) {
            if (outEdge->getDstNode() == dstNode) {
                edge = outEdge;
                break;
            }
        }
        
        if (!edge) {
            // No direct edge found, skip
            continue;
        }
        
        // Only process intra-procedural edges (skip call/return edges)
        if (!SVFUtil::isa<IntraCFGEdge>(edge)) {
            continue;
        }
        
        // Get branch condition for this edge
        // Check if srcBB has multiple successors (conditional branch)
        if (srcBB->getNumSuccessors() > 1) {
            // Check if dstBB is actually a successor of srcBB
            bool isSuccessor = false;
            for (const SVFBasicBlock* succ : srcBB->getSuccessors()) {
                if (succ == dstBB) {
                    isSuccessor = true;
                    break;
                }
            }
            
            if (isSuccessor) {
                // Only process conditional branches - check if the last node in srcBB is a branch instruction
                if (!srcBB->getICFGNodeList().empty()) {
                    const ICFGNode* branchNode = srcBB->back();
                    if (branchNode) {
                        // Check if this node has a conditional BranchStmt
                        // Only conditional branches have Z3 conditions
                        bool isConditionalBranch = false;
                        for (const SVFStmt* stmt : branchNode->getSVFStmts()) {
                            if (const BranchStmt* branchStmt = SVFUtil::dyn_cast<BranchStmt>(stmt)) {
                                // Only process conditional branches (unconditional branches don't have Z3 conditions)
                                if (branchStmt->isConditional()) {
                                    isConditionalBranch = true;
                                }
                                break;
                            }
                        }
                        
                        // Only proceed if this is a conditional branch instruction
                        if (isConditionalBranch) {
                            // Get all conditions associated with this branch node
                            auto condInfos = condAllocator->getConditionsForNode(branchNode);
                            
                            if (!condInfos.empty()) {
                                // Get the successor position to determine which condition to use
                                u32_t succPos = srcBB->getBBSuccessorPos(dstBB);
                                
                                SaberCondAllocator::Condition edgeCond = condAllocator->getTrueCond();
                                bool foundCond = false;
                                
                                // For 2-way branches (if/else), we typically have:
                                // - successor 0 (true branch) -> positive condition (isNeg = false)
                                // - successor 1 (false branch) -> negative condition (isNeg = true)
                                if (srcBB->getNumSuccessors() == 2 && condInfos.size() >= 2) {
                                    for (const auto& info : condInfos) {
                                        // Match based on successor position and negation flag
                                        if ((succPos == 0 && !info.isNeg) || (succPos == 1 && info.isNeg)) {
                                            edgeCond = info.cond;
                                            foundCond = true;
                                            break;
                                        }
                                    }
                                } else if (!condInfos.empty()) {
                                    // For other cases (switch, etc.), try to find matching condition
                                    // or use the first one as fallback
                                    for (const auto& info : condInfos) {
                                        edgeCond = info.cond;
                                        foundCond = true;
                                        break; // Use first condition found
                                    }
                                }
                                
                                if (foundCond) {
                                    // Check if condition is equivalent to TRUE
                                    bool isTrueCond = false;
                                    try {
                                        isTrueCond = condAllocator->isEquivalentBranchCond(edgeCond, condAllocator->getTrueCond());
                                    } catch (...) {
                                        isTrueCond = false;
                                    }
                                    
                                    if (!isTrueCond) {
                                        // Combine with existing path condition using AND
                                        // Wrap in try-catch to handle potential Z3 exceptions
                                        try {
                                            pathCond = condAllocator->condAnd(pathCond, edgeCond);
                                            hasCondition = true;
                                        } catch (const z3::exception& e) {
                                            // If Z3 throws an exception during AND operation,
                                            // skip this condition and continue
                                            continue;
                                        } catch (...) {
                                            // Catch any other exceptions
                                            continue;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    // If no conditions were found, return true condition (path is feasible)
    if (!hasCondition) {
        return condAllocator->getTrueCond();
    }
    
    return pathCond;
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

void PathQuery::getValueSensitiveReturnInsidePath(const std::string& startLocation, const std::vector<const SVFGNode*>& startSVFGNodes) {
    if (!icfg || !svfg || !pag) {
        GraphReaderUtil::sendJsonError("ICFG, SVFG, or PAG is null for value-sensitive analysis!");
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

    if (!startSVFGNodes.empty()) {
        for (const SVFGNode* startNodeCandidate : startSVFGNodes) {
            enqueueStartNode(startNodeCandidate);
        }

        if (validStartNodes.empty()) {
            GraphReaderUtil::sendJsonError("No valid SVFG start nodes belong to function '" + function->getName() + "'.");
            return;
        }
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

    PointerAnalysis* pta = svfg ? svfg->getPTA() : nullptr;
    Set<NodeID> startNodeLHSPointers;
    if (pta && !validStartNodes.empty()) {
        SVFUtil::errs() << "[ActualParmFilter] === Collecting start SVFGNode LHS pointer PAGNode IDs ===\n";
        for (const SVFGNode* startSVFGNode : validStartNodes) {
            const PAGNode* lhsNode = nullptr;
            if (const StmtVFGNode* stmtNode = SVFUtil::dyn_cast<StmtVFGNode>(startSVFGNode)) {
                lhsNode = stmtNode->getPAGDstNode();
            } else if (const FormalParmVFGNode* formalParmNode = SVFUtil::dyn_cast<FormalParmVFGNode>(startSVFGNode)) {
                lhsNode = formalParmNode->getParam();
            }
            if (lhsNode && lhsNode->isPointer()) {
                startNodeLHSPointers.insert(lhsNode->getId());
                SVFUtil::errs() << "[ActualParmFilter]   Added LHS pointer NodeID=" << lhsNode->getId()
                                << " from start SVFGNode ID=" << startSVFGNode->getId() << "\n";
            }
        }
        SVFUtil::errs() << "[ActualParmFilter]   Total start LHS pointer PAG nodes: " << startNodeLHSPointers.size() << "\n";
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
    std::map<const ICFGNode*, std::map<std::vector<NodeID>, std::vector<int>>> pathGroupsByReturn;

    for (const ICFGNode* retLocation : returnLocations) {
        std::vector<std::vector<const ICFGNode*>> pathsToThisReturn;
        findICFGPaths(startNode, retLocation, function, pathsToThisReturn);

        pathsByReturn[retLocation] = pathsToThisReturn;
        pathGroupsByReturn[retLocation];
    }

    // Step E & F: For each path, collect keySVFGNode sequence and group by sequence
    // Structure: return location -> (keySVFGNode sequence -> list of path indices)
    
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
                            
                            // Debug: show node type
                            if (SVFUtil::isa<StoreSVFGNode>(svfgNode)) {
                                SVFUtil::outs() << "[StoreFilter] Processing keySVFGNode ID=" << svfgNode->getId() 
                                                << ", Type=StoreSVFGNode\n";
                            }
                            
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
                            } else if (SVFUtil::isa<IntraMSSAPHISVFGNode>(svfgNode)) {
                                shouldHideInOutput = true;
                            } else if (SVFUtil::isa<LoadVFGNode>(svfgNode)) {
                                shouldHideInOutput = true;
                            } else if (SVFUtil::isa<CopyVFGNode>(svfgNode)) {
                                shouldHideInOutput = true;
                            } else if (const ActualParmVFGNode* actualParmNode = SVFUtil::dyn_cast<ActualParmVFGNode>(svfgNode)) {
                                SVFUtil::errs() << "[ActualParmFilter] Checking ActualParmVFGNode ID=" << actualParmNode->getId() << "\n";
                                const PAGNode* paramNode = actualParmNode->getParam();
                                if (!paramNode) {
                                    shouldHideInOutput = true;
                                    SVFUtil::errs() << "[ActualParmFilter]   -> FILTERED: Actual parameter has no associated PAG node\n";
                                } else if (startNodeLHSPointers.empty()) {
                                    SVFUtil::errs() << "[ActualParmFilter]   -> SKIPPED: No startSVFGNode LHS pointers available for reachability check\n";
                                } else {
                                    bool canReachStart = false;
                                    for (NodeID lhsPtrId : startNodeLHSPointers) {
                                        SVFUtil::errs() << "[ActualParmFilter]   Checking value flow: Actual param PAGNode ID=" << paramNode->getId()
                                                       << " -> Start LHS PAGNode ID=" << lhsPtrId << "\n";
                                        if (isValueFlowReachable(paramNode->getId(), lhsPtrId)) {
                                            canReachStart = true;
                                            SVFUtil::errs() << "[ActualParmFilter]     -> Value flow reachable. Keeping node.\n";
                                            break;
                                        }
                                    }
                                    if (!canReachStart) {
                                        shouldHideInOutput = true;
                                        SVFUtil::errs() << "[ActualParmFilter]   -> FILTERED: Actual parameter cannot reach any startSVFGNode LHS pointer\n";
                                    }
                                }
                            } else if (const StoreSVFGNode* storeNode = SVFUtil::dyn_cast<StoreSVFGNode>(svfgNode)) {
                                // Filter out stores to stack objects that are not address-taken
                                // This filters out local variable assignments like "mb = data"
                                // but keeps stores to function parameters like "*value = data"
                                SVFUtil::outs() << "[StoreFilter] Found StoreSVFGNode ID=" << svfgNode->getId() 
                                                << " (" << svfgNode->toString() << ")\n";
                                
                                const PAGNode* dstNode = storeNode->getPAGDstNode();
                                if (dstNode) {
                                    SVFUtil::outs() << "[StoreFilter]   DstNode ID=" << dstNode->getId() 
                                                    << ", Type=";
                                    if (SVFUtil::isa<ValVar>(dstNode)) {
                                        SVFUtil::outs() << "ValVar";
                                        if (SVFUtil::isa<ArgValVar>(dstNode)) {
                                            SVFUtil::outs() << "(ArgValVar)";
                                        }
                                    } else if (SVFUtil::isa<ObjVar>(dstNode)) {
                                        SVFUtil::outs() << "ObjVar";
                                    } else {
                                        SVFUtil::outs() << "Other";
                                    }
                                    SVFUtil::outs() << "\n";
                                    
                                    // For ValVar, check if it corresponds to a local variable (AllocaInst)
                                    // For ObjVar, check if it's a stack object
                                    bool isLocalStackVar = false;
                                    bool isAddressTaken = false;
                                    
                                    if (const ValVar* valVar = SVFUtil::dyn_cast<ValVar>(dstNode)) {
                                        // If it's an argument variable, it's address-taken (not a local variable)
                                        if (SVFUtil::isa<ArgValVar>(valVar)) {
                                            isAddressTaken = true;
                                            SVFUtil::outs() << "[StoreFilter]   -> ArgValVar detected, isAddressTaken=true\n";
                                        } else {
                                            // Check if the ValVar corresponds to an AllocaInst (local variable)
                                            const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(valVar);
                                            if (llvmVal) {
                                                if (SVFUtil::isa<llvm::AllocaInst>(llvmVal)) {
                                                    isLocalStackVar = true;
                                                    SVFUtil::outs() << "[StoreFilter]   -> ValVar corresponds to AllocaInst (local variable)\n";
                                                    
                                                    // Check if the address is truly taken (not just used in current Store)
                                                    // Address is taken if:
                                                    // 1. It's used in function calls (Call edges)
                                                    // 2. It's stored to other locations (Store edges to other nodes)
                                                    // 3. It has multiple AddrStmt edges (used in multiple places)
                                                    
                                                    bool hasCallEdges = valVar->hasOutgoingEdges(SVFStmt::Call);
                                                    bool hasStoreEdges = valVar->hasOutgoingEdges(SVFStmt::Store);
                                                    bool hasRetEdges = valVar->hasOutgoingEdges(SVFStmt::Ret);
                                                    
                                                    // Count AddrStmt edges
                                                    u32_t addrStmtCount = 0;
                                                    if (valVar->hasIncomingEdges(SVFStmt::Addr)) {
                                                        // Use const iterator to count edges
                                                        auto it = valVar->getIncomingEdgesBegin(SVFStmt::Addr);
                                                        auto end = valVar->getIncomingEdgesEnd(SVFStmt::Addr);
                                                        for (; it != end; ++it) {
                                                            addrStmtCount++;
                                                        }
                                                    }
                                                    
                                                    SVFUtil::outs() << "[StoreFilter]   -> AddrStmt count=" << addrStmtCount
                                                                    << ", hasCallEdges=" << (hasCallEdges ? "true" : "false")
                                                                    << ", hasStoreEdges=" << (hasStoreEdges ? "true" : "false")
                                                                    << ", hasRetEdges=" << (hasRetEdges ? "true" : "false") << "\n";
                                                    
                                                    // Address is taken if used in calls, returns, or multiple stores
                                                    if (hasCallEdges || hasRetEdges || (hasStoreEdges && addrStmtCount > 1)) {
                                                        isAddressTaken = true;
                                                        SVFUtil::outs() << "[StoreFilter]   -> Address is taken (used in calls/returns/multiple stores)\n";
                                                    } else {
                                                        SVFUtil::outs() << "[StoreFilter]   -> Address not taken (only used in local assignment)\n";
                                                    }
                                                } else {
                                                    SVFUtil::outs() << "[StoreFilter]   -> ValVar corresponds to non-AllocaInst: " 
                                                                    << (llvmVal->getName().empty() ? "unnamed" : llvmVal->getName().str()) << "\n";
                                                }
                                            } else {
                                                SVFUtil::outs() << "[StoreFilter]   -> ValVar has no corresponding LLVM value\n";
                                            }
                                        }
                                    } else if (const ObjVar* objVar = SVFUtil::dyn_cast<ObjVar>(dstNode)) {
                                        // Get the base object for ObjVar
                                        const BaseObjVar* baseObj = pag->getBaseObject(dstNode->getId());
                                        if (baseObj) {
                                            SVFUtil::outs() << "[StoreFilter]   BaseObj ID=" << baseObj->getId()
                                                            << ", isStack=" << (baseObj->isStack() ? "true" : "false")
                                                            << ", isHeap=" << (baseObj->isHeap() ? "true" : "false")
                                                            << ", isGlobal=" << (baseObj->isGlobalObj() ? "true" : "false")
                                                            << "\n";
                                            
                                            if (baseObj->isStack()) {
                                                isLocalStackVar = true;
                                                // Check if there are AddrStmt edges pointing to it
                                                bool hasAddrEdges = objVar->hasIncomingEdges(SVFStmt::Addr);
                                                SVFUtil::outs() << "[StoreFilter]   -> ObjVar, hasIncomingEdges(Addr)=" 
                                                                << (hasAddrEdges ? "true" : "false") << "\n";
                                                if (hasAddrEdges) {
                                                    isAddressTaken = true;
                                                }
                                            }
                                        } else {
                                            SVFUtil::outs() << "[StoreFilter]   -> BaseObj is null for ObjVar\n";
                                        }
                                    }
                                    
                                    SVFUtil::outs() << "[StoreFilter]   -> Final: isLocalStackVar=" 
                                                    << (isLocalStackVar ? "true" : "false")
                                                    << ", isAddressTaken=" << (isAddressTaken ? "true" : "false") << "\n";
                                    
                                    // Filter out stores to local stack variables that are not address-taken
                                    if (isLocalStackVar && !isAddressTaken) {
                                        shouldHideInOutput = true;
                                        SVFUtil::outs() << "[StoreFilter]   -> FILTERED OUT (local stack variable, address not taken)\n";
                                    } else {
                                        SVFUtil::outs() << "[StoreFilter]   -> KEPT (not a local stack variable or address taken)\n";
                                    }
                                } else {
                                    SVFUtil::outs() << "[StoreFilter]   -> DstNode is null\n";
                                }
                            }
                            
                            if (!shouldHideInOutput) {
                                keySVFGSequence.push_back(svfgNode->getId());
                                seenInPath.insert(svfgNode->getId());
                                if (SVFUtil::isa<StoreSVFGNode>(svfgNode)) {
                                    SVFUtil::outs() << "[StoreFilter]   -> ADDED to sequence (shouldHideInOutput=false)\n";
                                }
                            } else {
                                if (SVFUtil::isa<StoreSVFGNode>(svfgNode)) {
                                    SVFUtil::outs() << "[StoreFilter]   -> NOT added to sequence (shouldHideInOutput=true)\n";
                                }
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

void PathQuery::getValueSensitiveReturnInsidePathDetailed(const std::string& startLocation, const std::vector<const SVFGNode*>& startSVFGNodes) {
    if (!icfg || !svfg || !pag) {
        GraphReaderUtil::sendJsonError("ICFG, SVFG, or PAG is null for value-sensitive analysis!");
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

    // Step C: Use identifyKeySVFGNodesInFunction to get all key SVFG nodes
    // Collect key nodes from all valid start nodes
    std::set<const SVFGNode*> keySVFGNodesSet;
    std::vector<const SVFGNode*> validStartNodes;

    // Filter and collect valid start nodes
    for (const SVFGNode* startNodeCandidate : startSVFGNodes) {
        if (!startNodeCandidate) {
            continue;
        }
        if (startNodeCandidate->getFun() != function) {
            SVFUtil::errs() << "[getValueSensitiveReturnInsidePathDetailed] Ignoring start SVFGNode "
                            << startNodeCandidate->getId() << " because it belongs to function '"
                            << (startNodeCandidate->getFun() ? startNodeCandidate->getFun()->getName() : "unknown")
                            << "' instead of '" << function->getName() << "'.\n";
            continue;
        }
        validStartNodes.push_back(startNodeCandidate);
    }

    if (validStartNodes.empty()) {
        GraphReaderUtil::sendJsonError("No valid SVFG start nodes belong to function '" + function->getName() + "'.");
        return;
    }

    // Use identifyKeySVFGNodesInFunction for each start node and merge results
    for (const SVFGNode* startSVFGNode : validStartNodes) {
        std::set<const SVFGNode*> keyNodes = identifyKeySVFGNodesInFunction(function, startSVFGNode, true);
        keySVFGNodesSet.insert(keyNodes.begin(), keyNodes.end());
    }

    // Convert to Set for compatibility with existing code
    Set<const SVFGNode*> keySVFGNodes;
    for (const SVFGNode* node : keySVFGNodesSet) {
        keySVFGNodes.insert(node);
    }

    // Step D: Find all ICFG paths to return locations
    std::vector<const ICFGNode*> returnLocations = findActualReturnICFGNodes(icfg, function);

    if (returnLocations.empty()) {
        llvm::json::Object result;
        result["path_number"] = "0";
        result["paths"] = llvm::json::Array{};
        llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
        llvm::outs().flush();
        return;
    }

    // Find all ICFG paths to each return location
    std::map<const ICFGNode*, std::vector<std::vector<const ICFGNode*>>> pathsByReturn;

    // Get Z3 condition allocator for path feasibility checking
    SaberCondAllocator* condAllocator = GraphReaderUtil::getSaberCondAllocator();

    for (const ICFGNode* retLocation : returnLocations) {
        std::vector<std::vector<const ICFGNode*>> pathsToThisReturn;
        findICFGPaths(startNode, retLocation, function, pathsToThisReturn);
        
        // Filter paths using Z3 satisfiability checking
        if (condAllocator) {
            std::vector<std::vector<const ICFGNode*>> feasiblePaths;
            
            for (const auto& path : pathsToThisReturn) {
                // Collect path condition
                SaberCondAllocator::Condition pathCond = collectPathCondition(path, condAllocator);
                
                // Check if path is feasible (satisfiable)
                // For TRUE condition, it's always satisfiable, no need to call Z3
                bool isTrueCond = false;
                try {
                    isTrueCond = condAllocator->isEquivalentBranchCond(pathCond, condAllocator->getTrueCond());
                } catch (...) {
                    isTrueCond = false;
                }
                
                bool isSat = false;
                if (isTrueCond) {
                    isSat = true;
                } else {
                    // Wrap in try-catch to handle Z3 exceptions gracefully
                    try {
                        isSat = condAllocator->isSatisfiable(pathCond);
                    } catch (const z3::exception& e) {
                        // If Z3 throws an exception (e.g., invalid expression), skip this path
                        // This can happen if the condition is malformed or too complex
                        // In this case, we conservatively filter out the path
                        continue;
                    } catch (...) {
                        // Catch any other exceptions and skip this path
                        continue;
                    }
                }
                
                if (isSat) {
                    feasiblePaths.push_back(path);
                }
            }
            
            pathsByReturn[retLocation] = feasiblePaths;
        } else {
            // No condAllocator available, keep all paths
            pathsByReturn[retLocation] = pathsToThisReturn;
        }
    }

    // Step E & F: For each path, collect keySVFGNode sequence and group by sequence
    // Structure: return location -> (keySVFGNode sequence -> list of path indices)
    std::map<const ICFGNode*, std::map<std::vector<NodeID>, std::vector<int>>> pathGroupsByReturn;

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
                        // Check if this is a keySVFGNode (already filtered by identifyKeySVFGNodesInFunction)
                        if (keySVFGNodes.count(svfgNode) && !seenInPath.count(svfgNode->getId())) {
                            keySVFGSequence.push_back(svfgNode->getId());
                            seenInPath.insert(svfgNode->getId());
                        }
                    }
                }
            }
            
            // Group this path by its keySVFGNode sequence
            pathGroupsByReturn[retICFG][keySVFGSequence].push_back(pathIdx);
        }
    }

    // Step F.5: Extract and filter branch information for each path group
    // Structure: return ICFG -> (keySVFGSequence -> consistent branch nodes)
    // Branch node info: ICFG node, edge, location, condition value, position in path
    struct BranchNodeInfo {
        const ICFGNode* srcNode;
        const IntraCFGEdge* edge;
        llvm::json::Object branchInfo;
        size_t positionInPath;  // Index in ICFG path where this branch occurs
        std::string branchKey;  // Unique key: "location|condition_value|dst_node_id"
    };
    
    std::map<const ICFGNode*, 
             std::map<std::vector<NodeID>, 
                      std::vector<BranchNodeInfo>>> consistentBranchesByReturn;
    
    for (const auto& [retICFG, pathGroups] : pathGroupsByReturn) {
        const auto& icfgPaths = pathsByReturn.at(retICFG);
        
        for (const auto& [keySVFGSequence, pathIndices] : pathGroups) {
            if (pathIndices.empty()) {
                continue;
            }
            
            // Extract branch information from all paths in this group
            // Map: branchKey -> set of condition values across all paths
            std::map<std::string, std::set<std::string>> branchKeyToCondValues;
            // Map: branchKey -> BranchNodeInfo (for reference)
            std::map<std::string, BranchNodeInfo> branchKeyToInfo;
            
            for (int pathIdx : pathIndices) {
                if (pathIdx < 0 || static_cast<size_t>(pathIdx) >= icfgPaths.size()) {
                    continue;
                }
                const auto& icfgPath = icfgPaths[pathIdx];
                
                // Extract branches from this ICFG path
                for (size_t idx = 0; idx + 1 < icfgPath.size(); ++idx) {
                    const ICFGNode* srcNode = icfgPath[idx];
                    const ICFGNode* dstNode = icfgPath[idx + 1];
                    const IntraCFGEdge* branchEdge = nullptr;
                    
                    // Find the edge from srcNode to dstNode
                    for (ICFGEdge* edge : srcNode->getOutEdges()) {
                        if (edge->getDstNode() != dstNode) {
                            continue;
                        }
                        if (const IntraCFGEdge* intraEdge = SVFUtil::dyn_cast<IntraCFGEdge>(edge)) {
                            if (intraEdge->getCondition()) {
                                branchEdge = intraEdge;
                                break;
                            }
                        }
                    }
                    
                    if (!branchEdge) {
                        continue;
                    }
                    
                    // Create branch info
                    llvm::json::Object branchInfo = GraphReaderUtil::formatBranchInfo(branchEdge);
                    std::string location = "unknown";
                    std::string conditionValue = "unknown";
                    
                    if (auto loc = branchInfo.getString("location")) {
                        location = loc->str();
                    }
                    if (auto cond = branchInfo.getString("condition_value")) {
                        conditionValue = cond->str();
                    }
                    
                    // Create unique key: location + condition_value + dst_node_id
                    // This helps distinguish different branches at the same location
                    std::string branchKey = location + "|" + conditionValue + "|" + 
                                          std::to_string(dstNode->getId());
                    
                    // Store condition value for this branch key
                    branchKeyToCondValues[branchKey].insert(conditionValue);
                    
                    // Store branch info (use first occurrence as reference)
                    if (branchKeyToInfo.find(branchKey) == branchKeyToInfo.end()) {
                        BranchNodeInfo info;
                        info.srcNode = srcNode;
                        info.edge = branchEdge;
                        info.branchInfo = branchInfo;
                        info.positionInPath = idx;
                        info.branchKey = branchKey;
                        branchKeyToInfo[branchKey] = info;
                    }
                }
            }
            
            // Filter: only keep branches that are consistent across all paths in the group
            // A branch is consistent if ALL paths in the group have the same branch (same location, same condition value)
            std::vector<BranchNodeInfo> consistentBranches;
            for (const auto& [branchKey, condValues] : branchKeyToCondValues) {
                // If all paths have the same condition value, check if all paths have this branch
                if (condValues.size() == 1 && pathIndices.size() > 0) {
                    // Check if this branch appears in ALL paths with the same condition value
                    int pathCountWithBranch = 0;
                    
                    for (int pathIdx : pathIndices) {
                        if (pathIdx < 0 || static_cast<size_t>(pathIdx) >= icfgPaths.size()) {
                            continue;
                        }
                        const auto& icfgPath = icfgPaths[pathIdx];
                        
                        bool foundBranch = false;
                        for (size_t idx = 0; idx + 1 < icfgPath.size(); ++idx) {
                            const ICFGNode* srcNode = icfgPath[idx];
                            const ICFGNode* dstNode = icfgPath[idx + 1];
                            
                            for (ICFGEdge* edge : srcNode->getOutEdges()) {
                                if (edge->getDstNode() != dstNode) {
                                    continue;
                                }
                                if (const IntraCFGEdge* intraEdge = SVFUtil::dyn_cast<IntraCFGEdge>(edge)) {
                                    if (intraEdge->getCondition()) {
                                        llvm::json::Object brInfo = GraphReaderUtil::formatBranchInfo(intraEdge);
                                        std::string loc = "unknown";
                                        std::string condVal = "unknown";
                                        if (auto l = brInfo.getString("location")) {
                                            loc = l->str();
                                        }
                                        if (auto c = brInfo.getString("condition_value")) {
                                            condVal = c->str();
                                        }
                                        std::string key = loc + "|" + condVal + "|" + 
                                                         std::to_string(dstNode->getId());
                                        if (key == branchKey) {
                                            foundBranch = true;
                                            pathCountWithBranch++;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (foundBranch) {
                                break;
                            }
                        }
                    }
                    
                    // Only include branch if it appears in ALL paths in the group
                    // This ensures we only keep branches that are completely consistent across the group
                    if (pathCountWithBranch == static_cast<int>(pathIndices.size())) {
                        consistentBranches.push_back(branchKeyToInfo[branchKey]);
                    }
                }
            }
            
            // Sort branches by position in path (to maintain order)
            std::sort(consistentBranches.begin(), consistentBranches.end(),
                     [](const BranchNodeInfo& a, const BranchNodeInfo& b) {
                         return a.positionInPath < b.positionInPath;
                     });
            
            // Extract Z3 conditions from consistent branch nodes and verify feasibility
            // This is more efficient than verifying each path individually
            if (condAllocator && !consistentBranches.empty()) {
                SaberCondAllocator::Condition combinedCond = condAllocator->getTrueCond();
                bool hasValidCondition = false;
                bool isFeasible = true;
                
                for (const auto& branchInfo : consistentBranches) {
                    const ICFGNode* branchNode = branchInfo.srcNode;
                    if (!branchNode) {
                        continue;
                    }
                    
                    // Get the basic block for this branch node
                    const SVFBasicBlock* branchBB = branchNode->getBB();
                    if (!branchBB) {
                        continue;
                    }
                    
                    // Check if this is a conditional branch
                    bool isConditionalBranch = false;
                    for (const SVFStmt* stmt : branchNode->getSVFStmts()) {
                        if (const BranchStmt* branchStmt = SVFUtil::dyn_cast<BranchStmt>(stmt)) {
                            if (branchStmt->isConditional()) {
                                isConditionalBranch = true;
                                break;
                            }
                        }
                    }
                    
                    if (!isConditionalBranch) {
                        continue;
                    }
                    
                    // Get conditions for this branch node
                    auto condInfos = condAllocator->getConditionsForNode(branchNode);
                    if (condInfos.empty()) {
                        continue;
                    }
                    
                    // Determine which condition to use based on the edge
                    // Get the destination node from the edge
                    const ICFGNode* dstNode = branchInfo.edge->getDstNode();
                    if (!dstNode) {
                        continue;
                    }
                    
                    const SVFBasicBlock* dstBB = dstNode->getBB();
                    if (!dstBB || branchBB->getNumSuccessors() <= 1) {
                        continue;
                    }
                    
                    // Get successor position
                    u32_t succPos = branchBB->getBBSuccessorPos(dstBB);
                    SaberCondAllocator::Condition edgeCond = condAllocator->getTrueCond();
                    bool foundCond = false;
                    
                    // Match condition based on successor position
                    if (branchBB->getNumSuccessors() == 2 && condInfos.size() >= 2) {
                        for (const auto& info : condInfos) {
                            if ((succPos == 0 && !info.isNeg) || (succPos == 1 && info.isNeg)) {
                                edgeCond = info.cond;
                                foundCond = true;
                                break;
                            }
                        }
                    } else if (!condInfos.empty()) {
                        edgeCond = condInfos[0].cond;
                        foundCond = true;
                    }
                    
                    if (foundCond) {
                        // Check if condition is equivalent to TRUE
                        bool isTrueCond = false;
                        try {
                            isTrueCond = condAllocator->isEquivalentBranchCond(edgeCond, condAllocator->getTrueCond());
                        } catch (...) {
                            isTrueCond = false;
                        }
                        
                        if (!isTrueCond) {
                            // Combine with existing condition using AND
                            try {
                                combinedCond = condAllocator->condAnd(combinedCond, edgeCond);
                                hasValidCondition = true;
                            } catch (const z3::exception& e) {
                                // If Z3 throws an exception, mark as infeasible
                                isFeasible = false;
                                break;
                            } catch (...) {
                                isFeasible = false;
                                break;
                            }
                        }
                    }
                }
                
                // Verify satisfiability of combined condition
                if (hasValidCondition && isFeasible) {
                    bool isTrueCond = false;
                    try {
                        isTrueCond = condAllocator->isEquivalentBranchCond(combinedCond, condAllocator->getTrueCond());
                    } catch (...) {
                        isTrueCond = false;
                    }
                    
                    if (!isTrueCond) {
                        try {
                            isFeasible = condAllocator->isSatisfiable(combinedCond);
                        } catch (const z3::exception& e) {
                            isFeasible = false;
                        } catch (...) {
                            isFeasible = false;
                        }
                    }
                }
                
                // Filter out infeasible path groups
                if (!isFeasible) {
                    continue; // Skip this path group
                }
            }
            
            consistentBranchesByReturn[retICFG][keySVFGSequence] = std::move(consistentBranches);
        }
    }

    // Step G: Build JSON output with detailed path format
    // Each path group becomes one path in the output
    llvm::json::Array pathsArray;

    // Helper function to format location JSON object to "filename:line" string
    auto formatLocationString = [](const llvm::json::Object& locObj) -> std::string {
        std::string filename;
        int64_t line = 0;
        
        if (auto fl = locObj.getString("fl")) {
            filename = fl->str();
        }
        if (auto ln = locObj.getInteger("ln")) {
            line = *ln;
        }
        
        if (filename.empty() && line == 0) {
            return "";
        } else if (filename.empty()) {
            return std::to_string(line);
        } else if (line == 0) {
            return filename;
        } else {
            return filename + ":" + std::to_string(line);
        }
    };

    for (const auto& [retICFG, pathGroups] : pathGroupsByReturn) {
        // Get ICFG paths for this return location
        const auto& icfgPaths = pathsByReturn.at(retICFG);
        
        for (const auto& [keySVFGSequence, pathIndices] : pathGroups) {
            if (pathIndices.empty()) {
                continue;
            }
            
            llvm::json::Object pathObj;
            llvm::json::Array pathArray;
            
            // Add start node
            llvm::json::Object startNodeObj;
            startNodeObj["node"] = "start";
            
            std::string startNodeDesc = startNode->toString();
            if (startNodeDesc.empty()) {
                startNodeDesc = "Start location";
            }
            startNodeObj["node_desc"] = startNodeDesc;
            // Get location from startNode->toString() using parseSourceLocation and format as string
            llvm::json::Object startLocationObj = GraphReaderUtil::parseSourceLocation(startNodeDesc);
            startNodeObj["location"] = formatLocationString(startLocationObj);
            pathArray.push_back(std::move(startNodeObj));
            
            // Get consistent branches for this path group
            const auto& consistentBranches = consistentBranchesByReturn[retICFG][keySVFGSequence];
            
            // Build a unified sequence of nodes (branches and SVFG nodes) in path order
            // Use the first path in the group as reference for ordering
            struct PathNode {
                enum Type { BRANCH, SVFG };
                Type type;
                size_t position;  // Position in ICFG path
                llvm::json::Object nodeObj;
            };
            
            std::vector<PathNode> orderedNodes;
            
            // Add branch nodes
            for (const auto& branchInfo : consistentBranches) {
                PathNode node;
                node.type = PathNode::BRANCH;
                node.position = branchInfo.positionInPath;
                
                // Create branch node JSON object
                llvm::json::Object branchObj;
                branchObj["node"] = "branch";
                
                // Get branch description from ICFG node
                std::string branchDesc = branchInfo.srcNode->toString();
                if (branchDesc.empty()) {
                    branchDesc = "Branch";
                }
                branchObj["node_desc"] = branchDesc;
                
                // Get location from branch info
                std::string branchLocation = "unknown";
                if (auto loc = branchInfo.branchInfo.getString("location")) {
                    branchLocation = loc->str();
                }
                branchObj["location"] = branchLocation;
                
                // Add condition value
                std::string conditionValue = "unknown";
                if (auto cond = branchInfo.branchInfo.getString("condition_value")) {
                    conditionValue = cond->str();
                }
                branchObj["condition_value"] = conditionValue;
                
                node.nodeObj = std::move(branchObj);
                orderedNodes.push_back(std::move(node));
            }
            
            // Add SVFG nodes and determine their positions in ICFG path
            if (pathIndices[0] >= 0 && static_cast<size_t>(pathIndices[0]) < icfgPaths.size()) {
                const auto& refICFGPath = icfgPaths[pathIndices[0]];
                
                // Map: SVFG NodeID -> position in ICFG path
                std::map<NodeID, size_t> svfgNodeToPosition;
                for (size_t icfgIdx = 0; icfgIdx < refICFGPath.size(); ++icfgIdx) {
                    const ICFGNode* icfgNode = refICFGPath[icfgIdx];
                    // Find SVFG nodes at this ICFG location that are in keySVFGSequence
                    for (NodeID nodeId : keySVFGSequence) {
                        const SVFGNode* svfgNode = svfg->getSVFGNode(nodeId);
                        if (svfgNode && svfgNode->getICFGNode() == icfgNode) {
                            // Use first occurrence position
                            if (svfgNodeToPosition.find(nodeId) == svfgNodeToPosition.end()) {
                                svfgNodeToPosition[nodeId] = icfgIdx;
                            }
                        }
                    }
                }
                
                // Add SVFG nodes to ordered list
                for (NodeID nodeId : keySVFGSequence) {
                    const SVFGNode* svfgNode = svfg->getSVFGNode(nodeId);
                    if (!svfgNode) {
                        continue;
                    }
                    
                    PathNode node;
                    node.type = PathNode::SVFG;
                    // Get position from map, or use max if not found (shouldn't happen)
                    auto it = svfgNodeToPosition.find(nodeId);
                    node.position = (it != svfgNodeToPosition.end()) ? it->second : SIZE_MAX;
                    
                    llvm::json::Object nodeObj;
                    
                    // Get node type name
                    std::string nodeType = GraphReaderUtil::getSVFGNodeKindString(svfgNode, true);
                    nodeObj["node"] = nodeType;
                    
                    // Get node description
                    std::string nodeDesc = svfgNode->toString();
                    nodeObj["node_desc"] = nodeDesc;
                    
                    // Get location from svfgNode->toString() using parseSourceLocation and format as string
                    llvm::json::Object locationObj = GraphReaderUtil::parseSourceLocation(nodeDesc);
                    nodeObj["location"] = formatLocationString(locationObj);
                    
                    node.nodeObj = std::move(nodeObj);
                    orderedNodes.push_back(std::move(node));
                }
            } else {
                // Fallback: just add SVFG nodes in sequence order (no position info)
                for (NodeID nodeId : keySVFGSequence) {
                    const SVFGNode* svfgNode = svfg->getSVFGNode(nodeId);
                    if (!svfgNode) {
                        continue;
                    }
                    
                    PathNode node;
                    node.type = PathNode::SVFG;
                    node.position = SIZE_MAX;  // No position info, will be added at end
                    
                    llvm::json::Object nodeObj;
                    
                    // Get node type name
                    std::string nodeType = GraphReaderUtil::getSVFGNodeKindString(svfgNode, true);
                    nodeObj["node"] = nodeType;
                    
                    // Get node description
                    std::string nodeDesc = svfgNode->toString();
                    nodeObj["node_desc"] = nodeDesc;
                    
                    // Get location from svfgNode->toString() using parseSourceLocation and format as string
                    llvm::json::Object locationObj = GraphReaderUtil::parseSourceLocation(nodeDesc);
                    nodeObj["location"] = formatLocationString(locationObj);
                    
                    node.nodeObj = std::move(nodeObj);
                    orderedNodes.push_back(std::move(node));
                }
            }
            
            // Sort all nodes by position (branches and SVFG nodes together)
            std::sort(orderedNodes.begin(), orderedNodes.end(),
                     [](const PathNode& a, const PathNode& b) {
                         if (a.position != b.position) {
                             return a.position < b.position;
                         }
                         // If same position, branches come before SVFG nodes
                         if (a.type != b.type) {
                             return a.type < b.type;
                         }
                         return false;
                     });
            
            // Add ordered nodes to path
            for (auto& pathNode : orderedNodes) {
                pathArray.push_back(std::move(pathNode.nodeObj));
            }
            
            // Add return node
            llvm::json::Object returnNodeObj;
            returnNodeObj["node"] = "return";
            
            std::string retNodeDesc = retICFG->toString();
            if (retNodeDesc.empty()) {
                retNodeDesc = "Return location";
            }
            returnNodeObj["node_desc"] = retNodeDesc;
            // Get location from retICFG->toString() using parseSourceLocation and format as string
            llvm::json::Object retLocationObj = GraphReaderUtil::parseSourceLocation(retNodeDesc);
            returnNodeObj["location"] = formatLocationString(retLocationObj);
            pathArray.push_back(std::move(returnNodeObj));
            
            pathObj["path"] = std::move(pathArray);
            pathsArray.push_back(std::move(pathObj));
        }
    }

    // Build final result
    llvm::json::Object result;
    result["path_number"] = std::to_string(pathsArray.size());
    result["paths"] = std::move(pathsArray);

    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}

// dst - 关联我关心的变量 目标变量
// src - 当前位置的变量 - 可能指向一个 store(右值) 或者 formalparm(parm) 源变量
// 仅仅只沿着
bool PathQuery::isValueFlowReachable(NodeID src, NodeID dst) {
    if (!svfg || !pag) {
        return false;
    }
    
    if (src == dst) {
        return true;
    }
    
    // Step 1: Find all SVFG nodes that define dst (target nodes we want to reach)
    Set<const SVFGNode*> targetDefNodes;
    for (auto it = svfg->begin(); it != svfg->end(); ++it) {
        const SVFGNode* node = it->second;
        
        // Check if this node defines dst
        if (const StmtVFGNode* stmtNode = SVFUtil::dyn_cast<StmtVFGNode>(node)) {
            if (stmtNode->getPAGDstNode() && stmtNode->getPAGDstNode()->getId() == dst) {
                targetDefNodes.insert(node);
               
            }
        } else if (const FormalParmVFGNode* formalParmNode = SVFUtil::dyn_cast<FormalParmVFGNode>(node)) {
            if (formalParmNode->getParam() && formalParmNode->getParam()->getId() == dst) {
                targetDefNodes.insert(node);
            }
        }
    }
    
    if (targetDefNodes.empty()) {
        return false;
    }
    
    // Step 2: Find all SVFG nodes that use src (starting nodes for backward traversal)
    Set<const SVFGNode*> sourceUseNodes;

    for (auto it = svfg->begin(); it != svfg->end(); ++it) {
        const SVFGNode* node = it->second;
        
        // Check if this node uses src (as RHS/src or as LHS but from a different perspective)
        bool usesSrc = false;
        if (const StmtVFGNode* stmtNode = SVFUtil::dyn_cast<StmtVFGNode>(node)) {
            // Check if src is used as RHS (source) of this statement
            if (stmtNode->getPAGSrcNode() && stmtNode->getPAGSrcNode()->getId() == src) {
                usesSrc = true;
            }
            // For Store nodes, also check if src is the value being stored (RHS)
            // store to 也可以吗？
            if (const StoreSVFGNode* storeNode = SVFUtil::dyn_cast<StoreSVFGNode>(stmtNode)) {
                if (storeNode->getPAGSrcNode() && storeNode->getPAGSrcNode()->getId() == src) {
                    usesSrc = true;
                } else if (storeNode->getPAGDstNode() && storeNode->getPAGDstNode()->getId() == src) {
                    usesSrc = true;
                }
            }
        }
        
        if (usesSrc) {
            sourceUseNodes.insert(node);
        }
    }
    
    if (sourceUseNodes.empty()) {
        return false;
    }
    
    return backwardValueFlowReachable(sourceUseNodes, targetDefNodes);
}

bool PathQuery::backwardValueFlowReachable(const Set<const SVFGNode*>& seedNodes,
                                           const Set<const SVFGNode*>& targetDefNodes) {
    if (seedNodes.empty()) {
        return false;
    }

    std::queue<const SVFGNode*> worklist;
    Set<const SVFGNode*> visited;
    llvm::DenseMap<const SVFGNode*, const SVFGNode*> parentMap;

    for (const SVFGNode* node : seedNodes) {
        worklist.push(node);
        visited.insert(node);
        parentMap[node] = nullptr;
    }

    size_t iteration = 0;
    const size_t MAX_ITERATIONS = 1000; // Prevent infinite loops
    const size_t MAX_VISITED = 100; // Limit search space

    while (!worklist.empty() && iteration < MAX_ITERATIONS && visited.size() < MAX_VISITED) {
        iteration++;
        const SVFGNode* currentNode = worklist.front();
        worklist.pop();

        if (targetDefNodes.count(currentNode)) {
            std::vector<const SVFGNode*> pathNodes;
            const SVFGNode* pathNode = currentNode;
            while (pathNode) {
                pathNodes.push_back(pathNode);
                auto it = parentMap.find(pathNode);
                if (it == parentMap.end()) {
                    break;
                }
                pathNode = it->second;
            }

            return true;
        }

        if (SVFUtil::isa<IntraMSSAPHISVFGNode>(currentNode)) {
            continue;
        }

        for (const SVFGEdge* edge : currentNode->getInEdges()) {
            const SVFGNode* pred = edge->getSrcNode();

            if (pred && visited.insert(pred).second) {
                worklist.push(pred);
                parentMap[pred] = currentNode;
            }
        }
    }

    return false;
}

bool PathQuery::isLvarReachesReturn(SVFG* svfg, SVFIR* pag, const PAGNode* pagNode) {
    if (!svfg || !pag || !pagNode) {
        return false;
    }

    // Step 1: Get the def SVFGNode and function from the given PAGNode
    if (!svfg->hasDefSVFGNode(pagNode)) {
        return false;
    }

    const SVFGNode* defNode = svfg->getDefSVFGNode(pagNode);
    if (!defNode) {
        return false;
    }

    const FunObjVar* function = defNode->getFun();
    if (!function) {
        return false;
    }

    // Step 2: Early check - verify function return type is pointer type
    const llvm::Value* funVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(function);
    if (!funVal) {
        return false;
    }

    const llvm::Function* llvmFunc = llvm::dyn_cast<llvm::Function>(funVal);
    if (!llvmFunc) {
        return false;
    }

    // Check if return type is pointer type, if not (void, etc.), return false
    if (!llvmFunc->getReturnType()->isPointerTy()) {
        return false;
    }

    // Step 3: Find actual return locations using findActualReturnICFGNodes
    if (!icfg) {
        return false;
    }

    std::vector<const ICFGNode*> returnLocations = findActualReturnICFGNodes(icfg, function);
    if (returnLocations.empty()) {
        return false;
    }

    // Step 4: Extract LoadVFGNode statements at return locations and get their RHS PAGNodes
    std::vector<const SVFVar*> loadRHSNodes;
    
    for (const ICFGNode* retICFGNode : returnLocations) {
        // Find all SVFG nodes associated with this return ICFG node
        for (auto it = svfg->begin(); it != svfg->end(); ++it) {
            const SVFGNode* svfgNode = it->second;
            if (svfgNode->getICFGNode() != retICFGNode) {
                continue;
            }

            // Focus on LoadVFGNode
            if (const LoadVFGNode* loadNode = SVFUtil::dyn_cast<LoadVFGNode>(svfgNode)) {
                const SVFStmt* stmt = loadNode->getPAGEdge();
                if (!stmt) {
                    continue;
                }

                const LoadStmt* loadStmt = SVFUtil::dyn_cast<LoadStmt>(stmt);
                if (!loadStmt) {
                    continue;
                }

                // Get the RHS (right value) PAGNode - this is the address being loaded from
                const SVFVar* rhsVar = loadStmt->getRHSVar();
                if (rhsVar) {
                    loadRHSNodes.push_back(rhsVar);
                }
            }
        }
    }

    // Step 5: If no load operations found, return false
    if (loadRHSNodes.empty()) {
        return false;
    }

    // Step 6: Check if the starting PAGNode can reach any of the load RHS PAGNodes
    NodeID startNodeId = pagNode->getId();
    
    for (const SVFVar* loadRHSNode : loadRHSNodes) {
        if (!loadRHSNode) {
            continue;
        }

        NodeID loadRHSNodeId = loadRHSNode->getId();
        
        // Use isValueFlowReachable to check if startNode can reach loadRHSNode
        // Note: isValueFlowReachable(src, dst) checks if src can reach dst via backward traversal
        // We want to check if startNode can reach loadRHSNode, so we use isValueFlowReachable(startNodeId, loadRHSNodeId)
        if (isValueFlowReachable(startNodeId, loadRHSNodeId)) {
            return true;
        }
    }

    // If none of the load RHS nodes are reachable, return false
    return false;
}

std::set<const SVFGNode*> PathQuery::identifyKeySVFGNodesInFunction(const FunObjVar* function, const SVFGNode* startSVFGNode, bool isTool) {
    std::set<const SVFGNode*> result;
    
    // Validate inputs
    if (!function || !startSVFGNode || !svfg || !pag) {
        return result;
    }
    
    // Validate that start node belongs to the function
    if (startSVFGNode->getFun() != function) {
        return result;
    }
    
    // Step 1: BFS Collection - collect all reachable SVFG nodes within the function
    Set<const SVFGNode*> allReachableNodes;
    std::queue<const SVFGNode*> worklist;
    
    // Start BFS from the start node
    if (allReachableNodes.insert(startSVFGNode).second) {
        worklist.push(startSVFGNode);
    }
    
    // Perform BFS traversal
    while (!worklist.empty()) {
        const SVFGNode* currentNode = worklist.front();
        worklist.pop();
        
        // Traverse all out-edges
        for (const SVFGEdge* edge : currentNode->getOutEdges()) {
            const SVFGNode* nextNode = edge->getDstNode();
            // Only include nodes that belong to the specified function
            if (nextNode->getFun() != function) {
                continue;
            }
            if (allReachableNodes.insert(nextNode).second) {
                worklist.push(nextNode);
            }
        }
    }
    
    // Step 2: Compute dependencies - extract LHS pointer nodes from start node
    Set<NodeID> startNodeLHSPointers;
    PointerAnalysis* pta = svfg ? svfg->getPTA() : nullptr;
    
    if (pta) {
        const PAGNode* lhsNode = nullptr;
        
        // Get LHS node based on node type
        if (const StmtVFGNode* stmtNode = SVFUtil::dyn_cast<StmtVFGNode>(startSVFGNode)) {
            lhsNode = stmtNode->getPAGDstNode();
        } else if (const FormalParmVFGNode* formalParmNode = SVFUtil::dyn_cast<FormalParmVFGNode>(startSVFGNode)) {
            lhsNode = formalParmNode->getParam();
        }
        
        if (lhsNode && lhsNode->isPointer()) {
            startNodeLHSPointers.insert(lhsNode->getId());
        }
    }
    
    // Get the global set of all functions that call free
    const Set<std::string>& allFreeCallers = GraphReaderUtil::getAllFreeCallers();
    
    // Step 3: Apply filtering logic - iterate through all collected nodes
    for (const SVFGNode* svfgNode : allReachableNodes) {
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
        } else if (SVFUtil::isa<IntraMSSAPHISVFGNode>(svfgNode)) {
            shouldHideInOutput = true;
        } else if (SVFUtil::isa<LoadVFGNode>(svfgNode)) {
            shouldHideInOutput = true;
        } else if (SVFUtil::isa<CopyVFGNode>(svfgNode)) {
            shouldHideInOutput = true;
        } else if (const ActualINSVFGNode* actualInNode = SVFUtil::dyn_cast<ActualINSVFGNode>(svfgNode)) {
            std::vector<const StoreSVFGNode*> connectedStores;
            std::vector<const MSSAPHISVFGNode*> connectedMPHI;
            for (const SVFGEdge* inEdge : actualInNode->getInEdges()) {
                const SVFGNode* srcNode = inEdge->getSrcNode();
                if (const StoreSVFGNode* storeNode = SVFUtil::dyn_cast<StoreSVFGNode>(srcNode)) {
                    connectedStores.push_back(storeNode);
                } else if (const MSSAPHISVFGNode* mphiNode = SVFUtil::dyn_cast<MSSAPHISVFGNode>(srcNode)) {
                    connectedMPHI.push_back(mphiNode);
                }
            }
            shouldHideInOutput = true;
            std::queue<const MSSAPHISVFGNode*> mphiQueue;
            std::set<NodeID> processedMPHI;
            bool foundReachableStore = false;
            
            // 首先处理初始的connectedStores（如果有）
            for (const StoreSVFGNode* storeNode : connectedStores) {
                const SVFStmt* stmt = storeNode->getPAGEdge();
                const StoreStmt* storeStmt = SVFUtil::dyn_cast<StoreStmt>(stmt);
                if (!storeStmt) {
                    continue;
                }
                const SVFVar* lhsVar = storeStmt->getLHSVar();
                if (!lhsVar) {
                    continue;
                }
                NodeID lhsNodeId = lhsVar->getId();
                for (NodeID lhsPtrId : startNodeLHSPointers) {
                    if (isValueFlowReachable(lhsNodeId, lhsPtrId)) {
                        shouldHideInOutput = false;
                        foundReachableStore = true;
                        break;
                    }
                }
                if (foundReachableStore) {
                    break;
                }
            }
            
            // 将初始的connectedMPHI节点加入队列（如果还没找到isReachable的store）
            if (!foundReachableStore) {
                for (const MSSAPHISVFGNode* mphiNode : connectedMPHI) {
                    if (processedMPHI.find(mphiNode->getId()) == processedMPHI.end()) {
                        mphiQueue.push(mphiNode);
                        processedMPHI.insert(mphiNode->getId());
                    }
                }
            }
            
            // while循环处理所有mphi节点，直到找到isReachable的store节点或队列为空
            while (!mphiQueue.empty() && !foundReachableStore) {
                const MSSAPHISVFGNode* mphiNode = mphiQueue.front();
                mphiQueue.pop();
                
                // 遍历当前mphi节点的所有入边
                for (const SVFGEdge* inEdge : mphiNode->getInEdges()) {
                    if (foundReachableStore) {
                        break;
                    }
                    
                    const SVFGNode* srcNode = inEdge->getSrcNode();
                    
                    // 如果src节点是store类型的，立即处理并检查isReachable
                    if (const StoreSVFGNode* storeNode = SVFUtil::dyn_cast<StoreSVFGNode>(srcNode)) {
                        const SVFStmt* stmt = storeNode->getPAGEdge();
                        const StoreStmt* storeStmt = SVFUtil::dyn_cast<StoreStmt>(stmt);
                        if (storeStmt) {
                            const SVFVar* lhsVar = storeStmt->getLHSVar();
                            if (lhsVar) {
                                NodeID lhsNodeId = lhsVar->getId();
                                for (NodeID lhsPtrId : startNodeLHSPointers) {
                                    if (isValueFlowReachable(lhsNodeId, lhsPtrId)) {
                                        shouldHideInOutput = false;
                                        foundReachableStore = true;
                                        break;
                                    }
                                }
                            }
                        }
                    } 
                    // 如果src节点是mphi类型的，加入队列继续处理（如果还没找到isReachable的store）
                    else if (const MSSAPHISVFGNode* nextMphiNode = SVFUtil::dyn_cast<MSSAPHISVFGNode>(srcNode)) {
                        // 检查是否已经处理过，避免重复处理
                        if (processedMPHI.find(nextMphiNode->getId()) == processedMPHI.end()) {
                            mphiQueue.push(nextMphiNode);
                            processedMPHI.insert(nextMphiNode->getId());
                        }
                    }
                }
            }
            if (foundReachableStore) {
                while (!mphiQueue.empty()) {
                    mphiQueue.pop();
                }
            }
            
            // Check if the called function can call free
            // If it cannot call free, hide this node (only if we found a reachable store)
            if (!shouldHideInOutput && foundReachableStore) {
                const CallICFGNode* callSite = actualInNode->getCallSite();
                if (callSite) {
                    bool canCallFree = false;
                    const FunObjVar* directCallee = callSite->getCalledFunction();
                    if (directCallee) {
                        // Direct call: check if function name is in the global free callers set
                        canCallFree = (allFreeCallers.find(directCallee->getName()) != allFreeCallers.end());
                    } else {
                        // Indirect call: check all possible callees
                        const CallGraph* callGraph = pag->getCallGraph();
                        if (callGraph) {
                            CallGraph::FunctionSet callees;
                            const_cast<CallGraph*>(callGraph)->getCallees(callSite, callees);
                            for (const FunObjVar* callee : callees) {
                                if (callee) {
                                    if (allFreeCallers.find(callee->getName()) != allFreeCallers.end()) {
                                        canCallFree = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    // If the function cannot call free, hide this node
                    if (!canCallFree) {
                        shouldHideInOutput = true;
                    } else {
                        // If the function can call free, we should not hide it
                        shouldHideInOutput = false;
                    }
                }
            }
        } else if (const ActualParmVFGNode* actualParmNode = SVFUtil::dyn_cast<ActualParmVFGNode>(svfgNode)) {
            shouldHideInOutput = true;
            
            // Step 1: Check if parameter can reach our concerned variables
            bool canReachConcernedVar = false;
            std::vector<const LoadVFGNode*> connectedLoads;
            for (const SVFGEdge* inEdge : actualParmNode->getInEdges()) {
                const SVFGNode* pred = inEdge->getSrcNode();
                if (const LoadVFGNode* loadNode = SVFUtil::dyn_cast<LoadVFGNode>(pred)) {
                    connectedLoads.push_back(loadNode);
                }
            }
            for (const LoadVFGNode* loadNode : connectedLoads) {
                const SVFStmt* stmt = loadNode->getPAGEdge();
                const LoadStmt* loadStmt = SVFUtil::dyn_cast<LoadStmt>(stmt);
                if (!loadStmt) {
                    continue;
                }
                const SVFVar* rhsVar = loadStmt->getRHSVar();
                if (!rhsVar) {
                    continue;
                }
                NodeID rhsNodeId = rhsVar->getId();
                for (NodeID lhsPtrId : startNodeLHSPointers) {
                    if (isValueFlowReachable(rhsNodeId, lhsPtrId)) {
                        canReachConcernedVar = true;
                        break;
                    }
                }
                if (canReachConcernedVar) {
                    break;
                }
            }
            
            // Step 2: Only keep this node if parameter can reach concerned variables AND function can call free
            if (canReachConcernedVar) {
                const CallICFGNode* callSite = actualParmNode->getCallSite();
                if (callSite) {
                    bool canCallFree = false;
                    const FunObjVar* directCallee = callSite->getCalledFunction();
                    if (directCallee) {
                        // Direct call: check if function name is in the global free callers set
                        canCallFree = (allFreeCallers.find(directCallee->getName()) != allFreeCallers.end());
                    } else {
                        // Indirect call: check all possible callees
                        const CallGraph* callGraph = pag->getCallGraph();
                        if (callGraph) {
                            CallGraph::FunctionSet callees;
                            const_cast<CallGraph*>(callGraph)->getCallees(callSite, callees);
                            for (const FunObjVar* callee : callees) {
                                if (callee) {
                                    if (allFreeCallers.find(callee->getName()) != allFreeCallers.end()) {
                                        canCallFree = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    // Only keep node if both conditions are met
                    if (canCallFree) {
                        shouldHideInOutput = false;
                    }
                    // Otherwise, keep shouldHideInOutput = true (default)
                }
            }
            // If parameter cannot reach concerned variables, keep shouldHideInOutput = true (default)
        } else if (const GepVFGNode* gepNode = SVFUtil::dyn_cast<GepVFGNode>(svfgNode)) {
            // Filter out GEP operations on array objects
            // This operation doesn't affect memory ownership
            const GepStmt* gepStmt = SVFUtil::dyn_cast<GepStmt>(gepNode->getPAGEdge());
            if (gepStmt) {
                // Strategy 1: Check AccessPath types in GepStmt
                // AccessPath contains type information for each index operand
                bool isArrayFromAccessPath = false;
                bool hasStructFromAccessPath = false;
                const AccessPath::IdxOperandPairs& typePairs = gepStmt->getOffsetVarAndGepTypePairVec();
                for (const auto& pair : typePairs) {
                    const SVFType* idxType = pair.second;
                    if (idxType) {
                        if (SVFUtil::isa<SVFStructType>(idxType) || idxType->isStructTy()) {
                            hasStructFromAccessPath = true;
                        }
                        if (SVFUtil::isa<SVFArrayType>(idxType) || idxType->isArrayTy()) {
                            isArrayFromAccessPath = true;
                            break;
                        }
                    }
                }
                
                if (hasStructFromAccessPath) {
                    // Struct GEP should be preserved
                    shouldHideInOutput = false;
                } else if (isArrayFromAccessPath) {
                    shouldHideInOutput = true;
                } else {
                    shouldHideInOutput = true;
                }
                
                // Only continue with base object checks if not already filtered
                if (!shouldHideInOutput) {
                    
                    // Try to get base object from LHS (GepObjVar)
                    const PAGNode* lhsNode = gepStmt->getLHSVar();
                    const BaseObjVar* baseObj = nullptr;
                    
                    if (lhsNode) {
                        // Check if LHS is a GepObjVar
                        if (const GepObjVar* gepObjVar = SVFUtil::dyn_cast<GepObjVar>(lhsNode)) {
                            baseObj = gepObjVar->getBaseObj();
                        } else {
                            // Try getBaseObject for LHS
                            baseObj = pag->getBaseObject(lhsNode->getId());
                        }
                    }
                    
                    // If not found from LHS, try RHS
                    if (!baseObj) {
                        const PAGNode* rhsNode = gepStmt->getRHSVar();
                        if (rhsNode) {
                            // Check if RHS is an ObjVar directly
                            if (const ObjVar* objVar = SVFUtil::dyn_cast<ObjVar>(rhsNode)) {
                                if (const GepObjVar* gepObjVar = SVFUtil::dyn_cast<GepObjVar>(objVar)) {
                                    baseObj = gepObjVar->getBaseObj();
                                } else {
                                    baseObj = pag->getBaseObject(rhsNode->getId());
                                }
                            } else {
                                // RHS is ValVar (pointer value), try getBaseObject
                                baseObj = pag->getBaseObject(rhsNode->getId());
                            }
                        }
                    }
                    
                    // Check if base object is an array
                    if (baseObj) {
                        if (baseObj->isStruct()) {
                            shouldHideInOutput = false;
                        } else if (baseObj->isArray()) {
                            shouldHideInOutput = true;
                        }
                    } else {
                        // Try to check type from LHS node directly
                        bool isArrayType = false;
                        
                        if (lhsNode) {
                            // Get type from LHS node
                            const SVFType* lhsType = lhsNode->getType();
                            if (lhsType) {
                                // Check if it's an array type
                                if (SVFUtil::isa<SVFStructType>(lhsType) || lhsType->isStructTy()) {
                                    shouldHideInOutput = false;
                                    isArrayType = false;
                                } else if (SVFUtil::isa<SVFArrayType>(lhsType)) {
                                    isArrayType = true;
                                } else if (lhsType->isArrayTy()) {
                                    isArrayType = true;
                                }
                                
                                // Also try to get LLVM type if available
                                if (const ValVar* valVar = SVFUtil::dyn_cast<ValVar>(lhsNode)) {
                                    const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(valVar);
                                    if (llvmVal) {
                                        const llvm::Type* llvmType = llvmVal->getType();
                                        if (llvmType) {
                                            // Check if LLVM type is pointer to array
                                            if (llvmType->isPointerTy()) {
                                                // Handle opaque pointers in LLVM 16+
                                                const llvm::PointerType* ptrType = llvm::cast<llvm::PointerType>(llvmType);
                                                if (!ptrType->isOpaque()) {
                                                    const llvm::Type* pointeeType = ptrType->getNonOpaquePointerElementType();
                                                    if (pointeeType && pointeeType->isArrayTy()) {
                                                        isArrayType = true;
                                                    }
                                                } else {
                                                    // For opaque pointers, try to get type from GEP instruction if available
                                                    if (const llvm::GetElementPtrInst* gepInst = llvm::dyn_cast<llvm::GetElementPtrInst>(llvmVal)) {
                                                        const llvm::Type* sourceType = gepInst->getSourceElementType();
                                                        if (sourceType && sourceType->isArrayTy()) {
                                                            isArrayType = true;
                                                        }
                                                    }
                                                }
                                            } else if (llvmType->isArrayTy()) {
                                                isArrayType = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        
                        if (isArrayType) {
                            shouldHideInOutput = true;
                        }
                    }
                } // End of base object checks
            }
        } else if (const StoreSVFGNode* storeNode = SVFUtil::dyn_cast<StoreSVFGNode>(svfgNode)) {
            const PAGNode* dstNode = storeNode->getPAGDstNode();
            if (dstNode) {
                bool reachesFormalParm = GraphReaderUtil::isLvarFormalParm(svfg, pag, dstNode);
                bool reachesReturn = isLvarReachesReturn(svfg, pag, dstNode);
                // Only hide if left value neither reaches formal parameter nor reaches return value
                if (!reachesFormalParm && !reachesReturn) {
                    shouldHideInOutput = true;
                }
            }
            
            // Additional filter: filter out stores of concrete values (non-pointer) to objects of interest
            // This only affects memory values, not memory ownership/state
            if (!shouldHideInOutput) {
                const PAGNode* srcNode = storeNode->getPAGSrcNode();
                if (srcNode) {
                    if (!srcNode->isPointer()) {
                        shouldHideInOutput = true;
                    } else if (pta && !startNodeLHSPointers.empty()) {
                        bool hasAlias = false;
                        for (NodeID lhsPtrId : startNodeLHSPointers) {
                            if (pta->alias(srcNode->getId(), lhsPtrId) != AliasResult::NoAlias) {
                                hasAlias = true;
                                break;
                            }
                        }
                        
                        bool hasValueFlow = false;
                        if (!hasAlias) {
                            for (NodeID lhsPtrId : startNodeLHSPointers) {
                                if (isValueFlowReachable(srcNode->getId(), lhsPtrId)) {
                                    hasValueFlow = true;
                                    break;
                                }
                            }
                        }
                        
                        if (!hasAlias && !hasValueFlow) {
                            shouldHideInOutput = true;
                        }
                    }
                }
            }
        }
        
        // Add node to result if it passes all filters
        if (!shouldHideInOutput) {
            result.insert(svfgNode);
        }
    }
    
    // If not a tool function, output JSON format
    if (!isTool) {
        llvm::json::Object jsonResult;
        llvm::json::Array keySVFGsArray;
        
        // Helper function to format location JSON object to "filename:line" string
        auto formatLocationString = [](const llvm::json::Object& locObj) -> std::string {
            std::string filename;
            int64_t line = 0;
            
            if (auto fl = locObj.getString("fl")) {
                filename = fl->str();
            }
            if (auto ln = locObj.getInteger("ln")) {
                line = *ln;
            }
            
            if (filename.empty() && line == 0) {
                return "";
            } else if (filename.empty()) {
                return std::to_string(line);
            } else if (line == 0) {
                return filename;
            } else {
                return filename + ":" + std::to_string(line);
            }
        };
        
        // Build JSON array for each key SVFG node
        for (const SVFGNode* svfgNode : result) {
            llvm::json::Object nodeObj;
            
            // Get node type
            std::string nodeType = GraphReaderUtil::getSVFGNodeKindString(svfgNode, true);
            nodeObj["node_type"] = nodeType;
            
            // Get node description
            std::string nodeDesc = svfgNode->toString();
            nodeObj["node_desc"] = nodeDesc;
            
            // Get location from node description
            llvm::json::Object locationObj = GraphReaderUtil::parseSourceLocation(nodeDesc);
            std::string location = formatLocationString(locationObj);
            nodeObj["location"] = location;
            
            keySVFGsArray.push_back(std::move(nodeObj));
        }
        
        jsonResult["key_svfgs"] = std::move(keySVFGsArray);
        
        // Output JSON
        llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(jsonResult))) << "\n";
        llvm::outs().flush();
    }
    
    return result;
}

void PathQuery::findLvalueKeySVFGNodes(const std::string& location, int eqPosition) {
    if (!icfg || !svfg || !pag) {
        GraphReaderUtil::sendJsonError("ICFG, SVFG, or PAG is null!");
        return;
    }

    // Step 1: Find the PAGNode for the left value at the given location and eq_position
    const PAGNode* startPAGNode = GraphReaderUtil::getPAGNodeFromLvar(icfg, pag, location, eqPosition);
    if (!startPAGNode) {
        GraphReaderUtil::sendJsonError("Cannot find PAGNode for Lvar at location '" + location + "' with eq_position " + std::to_string(eqPosition));
        return;
    }

    // Step 2: Get the corresponding SVFGNode
    if (!svfg->hasDefSVFGNode(startPAGNode)) {
        GraphReaderUtil::sendJsonError("Cannot find SVFGNode for PAGNode " + std::to_string(startPAGNode->getId()));
        return;
    }

    const SVFGNode* startSVFGNode = svfg->getDefSVFGNode(startPAGNode);
    if (!startSVFGNode) {
        GraphReaderUtil::sendJsonError("SVFGNode is null for PAGNode " + std::to_string(startPAGNode->getId()));
        return;
    }

    // Step 3: Get the function from the start node
    const FunObjVar* function = startSVFGNode->getFun();
    if (!function) {
        GraphReaderUtil::sendJsonError("Start SVFGNode does not belong to any function");
        return;
    }

    // Step 4: Use identifyKeySVFGNodesInFunction with isTool=false to output JSON
    identifyKeySVFGNodesInFunction(function, startSVFGNode, false);
}

void PathQuery::findFormalArgKeySVFGNodes(const std::string& functionName, int argIndex) {
    if (!icfg || !svfg || !pag) {
        GraphReaderUtil::sendJsonError("ICFG, SVFG, or PAG is null!");
        return;
    }

    // Step 1: Find the PAGNode for the formal parameter at the given function name and arg_index
    const PAGNode* startPAGNode = GraphReaderUtil::getPAGNodeFromArg(pag, functionName, argIndex);
    if (!startPAGNode) {
        GraphReaderUtil::sendJsonError("Cannot find PAGNode for formal parameter at function '" + functionName + "' with arg_index " + std::to_string(argIndex));
        return;
    }

    // Step 2: Get the corresponding SVFGNode
    if (!svfg->hasDefSVFGNode(startPAGNode)) {
        GraphReaderUtil::sendJsonError("Cannot find SVFGNode for PAGNode " + std::to_string(startPAGNode->getId()));
        return;
    }

    const SVFGNode* startSVFGNode = svfg->getDefSVFGNode(startPAGNode);
    if (!startSVFGNode) {
        GraphReaderUtil::sendJsonError("SVFGNode is null for PAGNode " + std::to_string(startPAGNode->getId()));
        return;
    }

    // Step 3: Get the function from the start node
    const FunObjVar* function = startSVFGNode->getFun();
    if (!function) {
        GraphReaderUtil::sendJsonError("Start SVFGNode does not belong to any function");
        return;
    }

    // Step 4: Use identifyKeySVFGNodesInFunction with isTool=false to output JSON
    identifyKeySVFGNodesInFunction(function, startSVFGNode, false);
}

void PathQuery::findActualArgKeySVFGNodes(const std::string& location, const std::string& calleeFunctionName, int argIndex) {
    if (!icfg || !svfg || !pag) {
        GraphReaderUtil::sendJsonError("ICFG, SVFG, or PAG is null!");
        return;
    }

    // Step 1: Find the PAGNode for the actual argument at the given location and arg_index
    const PAGNode* startPAGNode = GraphReaderUtil::getPAGNodeFromCallArg(icfg, pag, location, argIndex, calleeFunctionName);
    if (!startPAGNode) {
        GraphReaderUtil::sendJsonError("Cannot find PAGNode for actual argument at location '" + location + "' with arg_index " + std::to_string(argIndex) + " for function '" + calleeFunctionName + "'");
        return;
    }

    // Step 2: Get CallICFGNode from location and function name
    std::vector<const ICFGNode*> allNodes = GraphReaderUtil::findAllICFGNodesByLocation(icfg, location);
    const CallICFGNode* callICFGNode = nullptr;
    
    for (const ICFGNode* node : allNodes) {
        if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node)) {
            const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(callNode);
            const llvm::CallBase* callInst = SVFUtil::dyn_cast<llvm::CallBase>(llvmVal);
            if (callInst) {
                const llvm::Function* directCallee = callInst->getCalledFunction();
                if (directCallee && directCallee->getName() == calleeFunctionName) {
                    callICFGNode = callNode;
                    break;
                }
                // Also check for indirect calls by checking the called operand name
                if (!directCallee) {
                    const llvm::Value* calledOperand = callInst->getCalledOperand();
                    if (calledOperand && calledOperand->hasName() && calledOperand->getName() == calleeFunctionName) {
                        callICFGNode = callNode;
                        break;
                    }
                }
            }
        }
    }
    
    if (!callICFGNode) {
        GraphReaderUtil::sendJsonError("Cannot find CallICFGNode at location '" + location + "' for function '" + calleeFunctionName + "'");
        return;
    }

    // Step 3: Check if ActualParmVFGNode exists for this PAGNode and CallICFGNode
    if (!svfg->hasActualParmVFGNode(startPAGNode, callICFGNode)) {
        GraphReaderUtil::sendJsonError("Cannot find ActualParmVFGNode for PAGNode " + std::to_string(startPAGNode->getId()) + " at call site");
        return;
    }

    // Step 4: Get the ActualParmVFGNode
    ActualParmVFGNode* actualParmNode = svfg->getActualParmVFGNode(startPAGNode, callICFGNode);
    if (!actualParmNode) {
        GraphReaderUtil::sendJsonError("ActualParmVFGNode is null for PAGNode " + std::to_string(startPAGNode->getId()));
        return;
    }

    // DEBUG: Output ActualParmVFGNode information
    // SVFUtil::outs() << "[findActualArgKeySVFGNodes] Found ActualParmVFGNode:\n";
    // SVFUtil::outs() << "  Node ID: " << actualParmNode->getId() << "\n";
    // SVFUtil::outs() << "  Node Description: " << actualParmNode->toString() << "\n";

    // Step 5: Get the caller function from the call site
    const FunObjVar* callerFunction = callICFGNode->getCaller();
    if (!callerFunction) {
        GraphReaderUtil::sendJsonError("CallICFGNode does not belong to any caller function");
        return;
    }

    // DEBUG: Output caller function information
    // SVFUtil::outs() << "[findActualArgKeySVFGNodes] Found caller function:\n";
    // SVFUtil::outs() << "  Function Name: " << callerFunction->getName() << "\n";
    // SVFUtil::outs() << "  Function ID: " << callerFunction->getId() << "\n";

    // Step 6: Determine the start SVFG node
    // Prefer def SVFG node of the PAG node if it exists, otherwise use actualParmNode
    const SVFGNode* startSVFGNode = nullptr;
    if (svfg->hasDefSVFGNode(startPAGNode)) {
        startSVFGNode = svfg->getDefSVFGNode(startPAGNode);
        if (startSVFGNode) {
            // SVFUtil::outs() << "[findActualArgKeySVFGNodes] Using def SVFG node for PAGNode " << startPAGNode->getId() << ":\n";
            // SVFUtil::outs() << "  Def SVFG Node ID: " << startSVFGNode->getId() << "\n";
            // SVFUtil::outs() << "  Def SVFG Node Description: " << startSVFGNode->toString() << "\n";
        } else {
            // SVFUtil::outs() << "[findActualArgKeySVFGNodes] Def SVFG node is null, falling back to ActualParmVFGNode\n";
            startSVFGNode = actualParmNode;
        }
    } else {
        // SVFUtil::outs() << "[findActualArgKeySVFGNodes] No def SVFG node found for PAGNode " << startPAGNode->getId() << ", using ActualParmVFGNode\n";
        startSVFGNode = actualParmNode;
    }

    // Step 7: Use identifyKeySVFGNodesInFunction with isTool=false to output JSON
    identifyKeySVFGNodesInFunction(callerFunction, startSVFGNode, false);
}

}// namespace SVF
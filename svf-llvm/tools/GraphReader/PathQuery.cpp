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
#include <llvm/ADT/DenseMap.h>
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

}// namespace SVF
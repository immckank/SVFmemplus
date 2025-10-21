#include "PathQuery.h"
#include "GraphReaderUtil.h"
#include "Graphs/ICFG.h"
#include "Graphs/SVFGEdge.h"
#include "SVFIR/SVFValue.h"
#include "Graphs/VFGNode.h"
#include "Graphs/SVFG.h"
#include "Util/SVFUtil.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <iostream>

namespace SVF {

void PathQuery::getValuePath(const SVFGNode* startNode) {
    if (!startNode) {
        SVFUtil::errs() << "Error: Start node is null.\n";
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
                        << " at " << node->getSourceLoc() << "\n";
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
    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
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
    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
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
                    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
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
    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
}

} // namespace SVF
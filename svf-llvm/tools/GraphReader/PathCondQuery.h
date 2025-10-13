// #ifndef PATH_COND_QUERY_H
// #define PATH_COND_QUERY_H

// #include "SABER/SrcSnkDDA.h"
// #include "Graphs/SVFG.h"
// #include "Graphs/ICFG.h"
// #include <llvm/Support/JSON.h>

// using namespace SVF;

// /*!
//  * \brief A query-based analyzer to find path conditions between a source and a sink.
//  *
//  * This class inherits from SrcSnkDDA and adapts it for a specific query
//  * between a user-defined start and target node.
//  */
// class PathCondQuery : public SrcSnkDDA {
// private:
//     const SVFGNode* startNode;
//     const SVFGNode* targetNode;
//     llvm::json::Object result;

// public:
//     PathCondQuery(const SVFGNode* start, const SVFGNode* target)
//         : SrcSnkDDA(), startNode(start), targetNode(target) {
//     }

//     virtual ~PathCondQuery() {}

//     /// Override initSrcs to set our specific start node as the only source.
//     void initSrcs() override {
//         if (startNode) {
//             addToSources(startNode);
//         }
//     }

//     /// Override initSnks to set our specific target node as the only sink.
//     void initSnks() override {
//         if (targetNode) {
//             addToSinks(targetNode);
//         }
//     }

//     /// Override reportBug to format and print the path conditions.
//     void reportBug(ProgSlice* slice) override {
//         llvm::json::Object pathInfo;
//         pathInfo["source_id"] = slice->getSource()->getId();

//         if (!slice->isPartialReachable()) {
//             pathInfo["reachable"] = false;
//             pathInfo["condition"] = "Target is not reachable from the source.";
//         } else {
//             pathInfo["reachable"] = true;
            
//             // Get the final path condition computed by the solver.
//             SaberCondAllocator::Condition finalCond = slice->getFinalCond();
            
//             // Use the allocator to get the elements (branch conditions) of the final condition.
//             NodeBS elems = getSaberCondAllocator()->exactCondElem(finalCond);
            
//             llvm::json::Array conditions;
//             for(NodeBS::iterator it = elems.begin(), eit = elems.end(); it != eit; ++it) {
//                 const ICFGNode* condInstNode = getSaberCondAllocator()->getCondInst(*it);
//                 bool isNeg = getSaberCondAllocator()->isNegCond(*it);

//                 std::string locString = condInstNode->getSourceLoc();
//                 std::string formattedLoc = locString;
//                 try {
//                     // Helper function from GraphReader.cpp
//                     std::string file = extract_value(locString, "fl");
//                     std::string line = extract_value(locString, "ln");
//                     formattedLoc = file + ":" + line;
//                 } catch (const std::runtime_error&) {}

//                 conditions.push_back(llvm::json::Object{
//                     {"location", formattedLoc},
//                     {"condition_value", !isNeg ? "true" : "false"}
//                 });
//             }
//             pathInfo["conditions"] = std::move(conditions);
//         }
//         result["path_analysis"] = std::move(pathInfo);
//     }

//     /// A custom analysis runner that only analyzes our single source.
//     void analyzeQuery() {
//         initialize();

//         if (getSources().empty()) {
//             result["error"] = true;
//             result["message"] = "Source node is not valid.";
//             return;
//         }

//         // The original `analyze` has a loop. We just need to process one source.
//         const SVFGNode* src = *sourcesBegin();
//         setCurSlice(src);

//         ContextCond cxt;
//         DPIm item(src->getId(), cxt);
//         forwardTraverse(item);

//         if (getCurSlice()->isPartialReachable()) {
//             backwardTraverse(DPIm(targetNode->getId(), cxt));
//             if(_curSlice->AllPathReachableSolve())
//                 _curSlice->setAllReachable();
//         }

//         reportBug(getCurSlice());
//     }

//     const llvm::json::Object& getResult() const {
//         return result;
//     }
// };

// #endif // PATH_COND_QUERY_H
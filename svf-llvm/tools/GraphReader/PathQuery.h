#ifndef PATH_QUERY_H
#define PATH_QUERY_H

#include "Graphs/ICFG.h"
#include "Graphs/SVFG.h"
#include "SABER/SaberCheckerAPI.h"
#include "SVF-LLVM/LLVMUtil.h"
#include <string>
#include <vector>
#include <set>

namespace SVF {

/*!
 * \class PathQuery
 * \brief Traverses value-flow paths in the SVFG.
 *
 * This class is designed to trace the flow of a value from a starting
 * SVFG node to its sinks (e.g., memory deallocation sites) or until
 * it's no longer used.
 */
class PathQuery {
public:
    using SVFGPath = std::vector<const SVFGNode*>;

    PathQuery(SVFG* s, ICFG* i) : svfg(s), icfg(i) {
        // Get the singleton instance of the checker API
        saberApi = SaberCheckerAPI::getCheckerAPI();
    }

    /*!
     * \brief Finds and prints all value-flow paths starting from a given node.
     *
     * This method traverses the SVFG to find all paths until the value is
     * passed to a deallocation function (e.g., free) or the path ends.
     * It then prints each path and classifies it as either "Safely Freed" or "Potential Leak".
     *
     * \param startNode The starting node in the SVFG for the traversal.
     */
    void getValuePath(const SVFGNode* startNode);

    /*!
     * \brief Traverses all paths from a start to a target location in the ICFG.
     *
     * This method performs a DFS on the ICFG to find all valid control-flow paths.
     * For each path, it reports conditional branches and any unreturned function calls.
     * The output is in JSON format.
     *
     * \param startLocation The starting source location string (e.g., "file.c:123").
     * \param targetLocation The target source location string.
     */
    void getConditionPath(const std::string& startLocation, const std::string& targetLocation);

    void getConditionInsidePath(const std::string& startLocation, const std::string& targetLocation);

private:
    SVFG* svfg;
    ICFG* icfg;
    SaberCheckerAPI* saberApi;

    void dfsVisit(const SVFGNode* currentNode, SVFGPath& currentPath, std::set<const SVFGNode*>& visited);
    void printPath(const SVFGPath& path, bool isFreed);
};

} // namespace SVF

#endif 
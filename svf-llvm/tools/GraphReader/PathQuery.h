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

    PathQuery(SVFG* s, ICFG* i) : svfg(s), icfg(i), pag(s ? s->getPAG() : nullptr) {
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

    void getValueInsidePath(const SVFGNode* startNode);

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

    //得到当前位置行代码的最近约束条件 如果没有返回no constrain
    void getConstrain(const std::string& location);

    /*!
     * \brief Finds and traces the value-flow path of a function argument within the function.
     *
     * This method locates a function by name, retrieves its argument list,
     * selects the argument at the given index, and then performs an intra-procedural
     * value-flow analysis starting from that argument's definition node.
     *
     * \param funcName The name of the function to analyze.
     * \param argIndex The 0-based index of the argument to trace.
     */
    void findFunArgValuePathInside(const std::string& funcName, int argIndex);

    /*!
     * \brief Finds and traces the value-flow path of a variable at a given location within the function.
     *
     * This method locates a variable at a specific source location by operand index,
     * finds its definition node in SVFG, and performs an intra-procedural value-flow analysis.
     *
     * \param location Source code location string (e.g., "file.c:123").
     * \param operandIndex The operand index: -1 for LHS (defined value), 0+ for RHS operands.
     */
    void findVarValuePathInsideByLocation(const std::string& location, int operandIndex);

    /*!
     * \brief Finds and traces the value-flow path of a local variable at a given location within the function.
     *
     * This method locates a local variable at a specific source location by operand index,
     * finds its definition node in SVFG, and performs an intra-procedural value-flow analysis.
     *
     * \param location Source code location string (e.g., "file.c:123").
     * \param operandIndex The operand index: -1 for LHS (defined value), 0+ for RHS operands.
     */
    void findLVarPathInsideByLocation(const std::string& location, int eqPosition);

private:
    SVFG* svfg;
    ICFG* icfg;
    SVFIR* pag;
    SaberCheckerAPI* saberApi;

    void dfsVisit(const SVFGNode* currentNode, SVFGPath& currentPath, std::set<const SVFGNode*>& visited);
    void printPath(const SVFGPath& path, bool isFreed);
};

} // namespace SVF

#endif 
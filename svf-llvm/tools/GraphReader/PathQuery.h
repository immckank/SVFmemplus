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

    /*!
     * \brief Finds all paths from a variable definition to FormalOUT nodes within the function.
     *
     * This method locates a variable at a specific source location (by line + column position),
     * finds its definition node in SVFG, and discovers all paths to the function's FormalOUT nodes.
     * Only MIntraPhi nodes are recorded along the paths.
     *
     * \param location Source code location string (e.g., "file.c:123").
     * \param eqPosition The column position of the assignment operator.
     */
    void findPathsToFormalOUT(const std::string& location, int eqPosition);

    /*!
     * \brief Finds all ICFG paths from a location to all function return locations.
     *
     * This method performs pure ICFG-level traversal (control flow) from the given location
     * to all actual return statements in the function. Unlike findPathsToFormalOUT, this does
     * not track value flow (SVFG) or memory states, only control flow paths.
     *
     * \param startLocation Source code location string (e.g., "file.c:123").
     */
    void getConditionReturnInsidePath(const std::string& startLocation);

    /*!
     * \brief Performs value-sensitive path merging from a location to function returns.
     *
     * This method combines control-flow analysis with value-flow analysis to determine
     * which paths are mergeable. Paths are considered mergeable if they reach the same
     * return location and have identical sequences of key SVFG nodes (nodes that affect
     * the target variable's memory). This enables more precise path-sensitive analysis
     * by distinguishing paths that make semantically different modifications to the
     * variable of interest.
     *
     * \param startLocation Source code location string (e.g., "file.c:123").
     * \param targetPAG The PAG node representing the variable of interest.
     */
    void getValueSensitiveReturnInsidePath(const std::string& startLocation, const SVFGNode* startSVFGNode);
    void getValueSensitiveReturnInsidePath(const std::string& startLocation,
                                           const std::vector<const SVFGNode*>& startSVFGNodes);
    void getValueSensitiveReturnInsidePath(const std::string& startLocation, const PAGNode* targetPAG);

    /*!
     * \brief Traces a function call argument's value flow to function returns.
     *
     * This method combines argument tracing with value flow analysis:
     * 1. Finds the definition point of the call argument (e.g., where it was allocated/stored)
     * 2. Traces all execution paths from the call site to function returns
     * 3. Performs value-sensitive path merging based on the argument's data flow
     *
     * \param callLocation Source location of the function call (e.g., "file.c:377").
     * \param argIndex The argument index (0-based, 0 is first argument).
     */
    void traceCallArgToReturn(const std::string& callLocation,
                              const std::string& functionName,
                              int argIndex);

private:
    SVFG* svfg;
    ICFG* icfg;
    SVFIR* pag;
    SaberCheckerAPI* saberApi;

    void dfsVisit(const SVFGNode* currentNode, SVFGPath& currentPath, std::set<const SVFGNode*>& visited);
    void printPath(const SVFGPath& path, bool isFreed);
    void getValueSensitiveReturnInsidePathImpl(const std::string& startLocation,
                                               const std::vector<const SVFGNode*>& startSVFGNodes,
                                               const PAGNode* targetPAG);

    /*!
     * \brief DFS helper to find paths from current node to a specific return location.
     *
     * \param currentNode Current node being visited
     * \param targetReturnICFG Target ICFG node representing an actual return location
     * \param function The function context (for intra-procedural traversal)
     * \param currentPath Current path of key nodes (source, MIntraPhis)
     * \param visited Set of visited nodes to avoid cycles
     * \param allPaths Collection of all found paths
     */
    void dfsToReturnLocation(
        const SVFGNode* currentNode,
        const ICFGNode* targetReturnICFG,
        const FunObjVar* function,
        std::vector<const SVFGNode*>& currentPath,
        std::set<const SVFGNode*>& visited,
        std::vector<std::vector<const SVFGNode*>>& allPaths
    );
};

} // namespace SVF

#endif 
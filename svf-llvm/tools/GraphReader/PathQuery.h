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
     * \brief Collects branch constraints from a function's entry to a target location.
     *
     * This method enumerates all intra-procedural paths starting at the function entry
     * and ending at the given location. For each path, it records conditional branches
     * encountered and whether the true or false edge was taken. The result also reports
     * common constraints that every discovered path must satisfy.
     *
     * \param location Target source location string (e.g., "file.c:123").
     */
    void getConstrainInside(const std::string& location);

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
    void getValueSensitiveReturnInsidePath(const std::string& startLocation,
                                           const std::vector<const SVFGNode*>& startSVFGNodes);

    void findICFGPaths(const ICFGNode* startICFG,
                       const ICFGNode* targetICFG,
                       const FunObjVar* function,
                       std::vector<std::vector<const ICFGNode*>>& allICFGPaths);
private:
    SVFG* svfg;
    ICFG* icfg;
    SVFIR* pag;
    SaberCheckerAPI* saberApi;
};

} // namespace SVF

#endif 
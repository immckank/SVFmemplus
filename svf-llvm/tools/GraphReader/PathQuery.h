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

    /*!
     * \brief Performs value-sensitive path analysis with detailed path output format.
     *
     * This method has the same logic as getValueSensitiveReturnInsidePath but outputs
     * paths in a detailed format where each path includes start node, key SVFG nodes,
     * and return node with full location information.
     *
     * \param startLocation Source code location string (e.g., "file.c:123").
     * \param startSVFGNodes Optional vector of starting SVFG nodes.
     */
    void getValueSensitiveReturnInsidePathDetailed(const std::string& startLocation,
                                                   const std::vector<const SVFGNode*>& startSVFGNodes);

    void findICFGPaths(const ICFGNode* startICFG,
                       const ICFGNode* targetICFG,
                       const FunObjVar* function,
                       std::vector<std::vector<const ICFGNode*>>& allICFGPaths);

    /*!
     * \brief Check if there is a value flow path from src PAGNode to dst PAGNode using backward traversal.
     *
     * This method performs backward traversal on SVFG starting from nodes that use src,
     * and checks if we can reach any node that defines dst. It outputs debug information
     * for each node visited during traversal.
     *
     * \param src Source PAGNode ID (the value being used).
     * \param dst Target PAGNode ID (the value we want to reach).
     * \return true if there is a value flow path, false otherwise.
     */
    bool isValueFlowReachable(NodeID src, NodeID dst);

    /*!
     * \brief Check if a left value can reach the return value of the current function.
     *
     * This method checks if the given PAGNode (left value) can traverse forward
     * to reach the return value of the function containing it. It finds actual return
     * locations using findActualReturnICFGNodes, extracts LoadVFGNode statements at
     * those locations, and checks if the PAGNode can reach the RHS (right value) of
     * those load statements using isValueFlowReachable.
     *
     * \param svfg Pointer to the SVFG.
     * \param pag Pointer to the SVFIR/PAG.
     * \param pagNode The PAG node to check (left value).
     * \return true if the node can reach a return value load's RHS, false otherwise.
     *         Returns false if function return type is not pointer, or if no load
     *         operations are found at return locations.
     */
    bool isLvarReachesReturn(SVFG* svfg, SVFIR* pag, const PAGNode* pagNode);

    /*!
     * \brief Identifies all key SVFG nodes within a function that are reachable from a start node.
     *
     * This method performs BFS traversal from the start SVFG node to collect all reachable
     * SVFG nodes within the specified function, then applies filtering logic to determine
     * which nodes are "key" nodes (nodes that affect memory/data flow).
     *
     * \param function The function object representing the function to search within.
     * \param startSVFGNode The starting SVFG node for BFS traversal.
     * \param isTool If true, function is used as a tool function and returns the result set.
     *               If false, function outputs a JSON with format:
     *               {"key_svfgs": [{"node_type": "...", "node_id": 123, "location": "...", "node_desc": "..."}, ...]}
     * \return A set of key SVFG nodes that pass all filtering criteria.
     */
    std::set<const SVFGNode*> identifyKeySVFGNodesInFunction(
        const FunObjVar* function,
        const SVFGNode* startSVFGNode,
        bool isTool = true,
        const std::vector<std::string>& offsets = {}
    );

    /*!
     * \brief Finds key SVFG nodes for a left value at a specific location and equation position.
     *
     * This method finds the PAGNode for a left value at the given location and eq_position,
     * gets its corresponding SVFGNode, and then uses identifyKeySVFGNodesInFunction to
     * identify all key SVFG nodes within the function.
     *
     * \param location Source code location string (e.g., "file.c:123").
     * \param eqPosition The equation position (column) to match.
     * \param offsets Optional list of GEP offsets to filter nodes (e.g., ["1", "2"]).
     */
    void findLvalueKeySVFGNodes(const std::string& location, int eqPosition, const std::vector<std::string>& offsets = {});

    /*!
     * \brief Finds key SVFG nodes for a formal parameter at a specific function and argument index.
     *
     * This method finds the PAGNode for a formal parameter at the given function name and arg_index,
     * gets its corresponding SVFGNode, and then uses identifyKeySVFGNodesInFunction to
     * identify all key SVFG nodes within the function.
     *
     * \param functionName The name of the function.
     * \param argIndex The argument index (0-based) of the formal parameter.
     * \param offsets Optional list of GEP offsets to filter nodes (e.g., ["1", "2"]).
     */
    void findFormalArgKeySVFGNodes(const std::string& functionName, int argIndex, const std::vector<std::string>& offsets = {});

    /*!
     * \brief Finds key SVFG nodes for an actual argument at a specific call site.
     *
     * This method finds the PAGNode for an actual argument at the given location, callee function name,
     * and arg_index, gets its corresponding ActualParmVFGNode, and then uses identifyKeySVFGNodesInFunction
     * to identify all key SVFG nodes within the caller function.
     *
     * \param location Source code location string (e.g., "file.c:123") of the call site.
     * \param calleeFunctionName The name of the called function.
     * \param argIndex The argument index (0-based) of the actual parameter.
     * \param offsets Optional list of GEP offsets to filter nodes (e.g., ["1", "2"]).
     */
    void findActualArgKeySVFGNodes(const std::string& location, const std::string& calleeFunctionName, int argIndex, const std::vector<std::string>& offsets = {});

private:

    bool backwardValueFlowReachable(const Set<const SVFGNode*>& seedNodes,
                                    const Set<const SVFGNode*>& targetDefNodes);

    SVFG* svfg;
    ICFG* icfg;
    SVFIR* pag;
    SaberCheckerAPI* saberApi;
};

/*!
 * \brief Helper function to find actual return statement ICFG nodes (not wrapper nodes).
 * 
 * This function finds all actual return locations in a function by traversing the ICFG
 * and identifying return instruction nodes, then following their predecessors to find
 * the actual return statements (which may be unconditional branches before return instructions).
 * 
 * \param icfg Pointer to the ICFG.
 * \param function Pointer to the FunObjVar representing the function.
 * \return A vector of ICFG nodes representing actual return locations.
 */
std::vector<const ICFGNode*> findActualReturnICFGNodes(ICFG* icfg, const FunObjVar* function);

} // namespace SVF

#endif 

#ifndef GRAPH_READER_UTIL_H
#define GRAPH_READER_UTIL_H

#include "Graphs/ICFG.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFVariables.h"
#include "SVFIR/SVFValue.h"
#include "Graphs/SVFG.h"
#include "Graphs/SVFGNode.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/JSON.h>
#include <string>
#include <vector>

namespace SVF {

class SVFIR; // Forward declaration
class SVFG;  // Forward declaration

namespace GraphReaderUtil {

    /*!
     * \brief Finds an ICFGNode based on a source code location string.
     * \param icfg Pointer to the ICFG.
     * \param location A string in "filename:line" format.
     * \return A const pointer to the matched ICFGNode, or nullptr if not found.
     */
    const ICFGNode* findICFGNodeByLocation(const ICFG* icfg, const std::string& location);

    /*!
     * \brief Finds ALL ICFGNodes matching a source code location string.
     * \param icfg Pointer to the ICFG.
     * \param location A string in "filename:line" format.
     * \return A vector of all matching ICFGNodes.
     */
    std::vector<const ICFGNode*> findAllICFGNodesByLocation(const ICFG* icfg, const std::string& location);
 

    /*!
     * \brief Parses an SVF source location string into a JSON object.
     * This is a robust replacement for the old manual string parsing.
     */
    llvm::json::Object parseSourceLocation(const std::string& sourceLocString);

    /*!
     * \brief Converts SVFGNode kind to a string representation.
     * \param node Pointer to the SVFGNode.
     * \param detailed When true, returns detailed class names (e.g. StoreVFGNode).
     * \return A string describing the node kind.
     */
    std::string getSVFGNodeKindString(const SVFGNode* node, bool detailed = false);

    /*!
     * \brief Creates and prints a standardized JSON error response.
     * \param message The error message to include in the JSON output.
     */
    void sendJsonError(const std::string& message);

    /*!
     * \brief Creates a JSON object containing a function's source information.
     * \param llvmFun Pointer to the LLVM function.
     * \return A llvm::json::Object with function_name, filename, start_line, and end_line.
     */
    llvm::json::Object getFunctionInfoJson(const llvm::Function* llvmFun);

    /*!
     * \brief Formats an IntraCFGEdge's branch information into a JSON object.
     * \param intraEdge The edge to format.
     * \return A llvm::json::Object with type, location, and condition_value.
     */
    llvm::json::Object formatBranchInfo(const IntraCFGEdge* intraEdge);

    /*!
     * \brief Gets store operation column information at a given source location.
     * \param svfg Pointer to the SVFG.
     * \param icfg Pointer to the ICFG.
     * \param location A string in "filename:line" format.
     * \return A llvm::json::Object with location and store_cl array containing column numbers.
     */
    llvm::json::Object getStoreClInfoJson(SVFG* svfg, ICFG* icfg, const std::string& location);

    llvm::json::Object getGepClInfoJson(SVFG* svfg, ICFG* icfg, const std::string& location);

    /// Parse a JSON string into a list of command objects.
    /// Supports: an array of objects; a single object; or an object with a
    /// top-level "commands" array. Returns true on success; otherwise false and
    /// sets errMsg.
    bool parseCommandsLine(const std::string& jsonStr, std::vector<llvm::json::Object>& outCmds, std::string& errMsg);

    bool fetchFunctionStartLocation(SVFIR* pag, const std::string& funcName, std::string& startLocation);

    /*!
     * \brief Gets the PAGNode for a specific function argument.
     * \param pag Pointer to the SVFIR/PAG.
     * \param funcName The name of the function.
     * \param argIndex The index of the argument (0-based).
     * \return A const pointer to the PAGNode of the argument, or nullptr if not found.
     */
    const PAGNode* getPAGNodeFromArg(SVFIR* pag, const std::string& funcName, int argIndex);

    /*!
     * \brief Gets the LHS PAGNode from a source location and equation position.
     * \param icfg Pointer to the ICFG.
     * \param pag Pointer to the SVFIR/PAG.
     * \param location A string in "filename:line" format.
     * \param eqPosition The equation position (column) to match.
     * \return A const pointer to the LHS PAGNode, or nullptr if not found.
     */
    const PAGNode* getPAGNodeFromLvar(ICFG* icfg, SVFIR* pag, const std::string& location, int eqPosition);

    /*!
     * \brief Gets the base PAGNode from a GEP operation at a source location and equation position.
     * This function finds GEP statements and returns the base object (RHS) rather than the result (LHS).
     * If the base is itself a GepValVar, it recursively extracts the ultimate base object.
     * \param icfg Pointer to the ICFG.
     * \param pag Pointer to the SVFIR/PAG.
     * \param location A string in "filename:line" format.
     * \param eqPosition The equation position (column) to match.
     * \return A const pointer to the base PAGNode, or nullptr if not found.
     */
    const PAGNode* getPAGNodeFromLvarGEP(ICFG* icfg, SVFIR* pag, const std::string& location, int eqPosition);

    /*!
     * \brief Gets the PAGNode for a call argument at a specific location.
     * \param icfg Pointer to the ICFG.
     * \param pag Pointer to the SVFIR/PAG.
     * \param location A string in "filename:line" format for the call site.
     * \param argIndex The argument index (0-based).
     * \return A const pointer to the PAGNode of the argument, or nullptr if not found.
     */
    const PAGNode* getPAGNodeFromCallArg(ICFG* icfg, SVFIR* pag, const std::string& location, int argIndex, const std::string& functionName = "");


    /*!
     * \brief Recursively traces the definition chain of a PAGNode to its ultimate source.
     * Follows LoadVFGNode → Store operations until reaching an ultimate source like
     * FormalParmVFGNode, AddrVFGNode, or ActualRetVFGNode.
     * Returns both the direct definition and the ultimate source in JSON format.
     * \param svfg Pointer to the SVFG.
     * \param pag Pointer to the SVFIR/PAG.
     * \param pagNode The PAGNode to query.
     */
    void tracePAGStore(SVFG* svfg, SVFIR* pag, const SVFVar* pagNode);

    /*!
     * \brief Shows all ICFG nodes and their corresponding SVFG nodes at a given source location.
     * \param svfg Pointer to the SVFG.
     * \param icfg Pointer to the ICFG.
     * \param location A string in "filename:line" format.
     */
    void showCodeLineDebugInfo(SVFG* svfg, ICFG* icfg, const std::string& location);

    /*!
     * \brief Traces a call argument's value flow and finds its definition point.
     * Combines argument tracing with backward data flow to find where the value was stored.
     * Returns the PAGNode of the stored value (e.g., malloc result).
     * \param svfg Pointer to the SVFG.
     * \param icfg Pointer to the ICFG.
     * \param pag Pointer to the SVFIR/PAG.
     * \param callLocation Source location of the function call.
     * \param argIndex The argument index (0-based).
     * \return The PAGNode representing the value's definition, or nullptr if not found.
     */
    const PAGNode* tracePAGNodeFromCallArg(SVFG* svfg, ICFG* icfg, SVFIR* pag, const std::string& callLocation, const std::string& functionName, int argIndex);

} // namespace GraphReaderUtil
} // namespace SVF

#endif // GRAPH_READER_UTIL_H
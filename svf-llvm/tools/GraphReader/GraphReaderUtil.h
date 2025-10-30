#ifndef GRAPH_READER_UTIL_H
#define GRAPH_READER_UTIL_H

#include "Graphs/ICFG.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFVariables.h"
#include "SVFIR/SVFValue.h"
#include "Graphs/SVFG.h"
#include "Graphs/SVFGNode.h"
#include <llvm/IR/Function.h>
#include <llvm/Support/JSON.h>
#include <string>
#include <vector>

namespace SVF {

class SVFIR; // Forward declaration
class SVFG;  // Forward declaration

/// @struct FunctionSourceInfo
/// @brief Encapsulates source code information extracted from LLVM debug info.
struct FunctionSourceInfo {
    std::string filename;
    unsigned startLine;
    unsigned endLine;
};

namespace GraphReaderUtil {

    /*!
     * \brief Finds an ICFGNode based on a source code location string.
     * \param icfg Pointer to the ICFG.
     * \param location A string in "filename:line" format.
     * \return A const pointer to the matched ICFGNode, or nullptr if not found.
     */
    const ICFGNode* findICFGNodeByLocation(const ICFG* icfg, const std::string& location);

    /*!
     * \brief Finds the source code variable name for a given SVFVar by inspecting debug info.
     * \param var The SVFVar to inspect.
     * \return The source code name as a string, or an empty string if not found.
     */
    std::string getSourceVariableName(const SVFVar* var);


    std::vector<const SVFVar*> findVarByLocation(const SVFIR* pag, const std::string& location);

    std::vector<const SVFVar*> findDefinedVarByLocation(const SVFIR* pag, const SVFG* svfg, const std::string& location);
    
    /*!
     * \brief Gets the source file info (name, start/end line) of an llvm::Function.
     */
    FunctionSourceInfo getFunctionSourceInfo(const llvm::Function* llvmFun);

    /*!
     * \brief Parses an SVF source location string into a JSON object.
     * This is a robust replacement for the old manual string parsing.
     */
    llvm::json::Object parseSourceLocation(const std::string& sourceLocString);

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

    /// Parse a JSON string into a list of command objects.
    /// Supports: an array of objects; a single object; or an object with a
    /// top-level "commands" array. Returns true on success; otherwise false and
    /// sets errMsg.
    bool parseCommandsLine(const std::string& jsonStr,
                           std::vector<llvm::json::Object>& outCmds,
                           std::string& errMsg);

} // namespace GraphReaderUtil
} // namespace SVF

#endif // GRAPH_READER_UTIL_H
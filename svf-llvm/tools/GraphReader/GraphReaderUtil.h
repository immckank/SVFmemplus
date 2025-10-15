#ifndef GRAPH_READER_UTIL_H
#define GRAPH_READER_UTIL_H

#include "Graphs/ICFG.h"
#include <llvm/IR/Function.h>
#include <llvm/Support/JSON.h>
#include <string>

namespace SVF {

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
     * \brief Gets the source file info (name, start/end line) of an llvm::Function.
     */
    FunctionSourceInfo getFunctionSourceInfo(const llvm::Function* llvmFun);

    /*!
     * \brief Parses an SVF source location string into a JSON object.
     * This is a robust replacement for the old manual string parsing.
     */
    llvm::json::Object parseSourceLocation(const std::string& sourceLocString);

} // namespace GraphReaderUtil
} // namespace SVF

#endif // GRAPH_READER_UTIL_H
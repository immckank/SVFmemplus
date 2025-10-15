#include "GraphReaderUtil.h"
#include "SVF-LLVM/LLVMUtil.h"
#include <llvm/Support/FormatVariadic.h>
#include <llvm/IR/DebugInfo.h>

namespace SVF {
namespace GraphReaderUtil {

llvm::json::Object parseSourceLocation(const std::string& sourceLocString) {
    if (sourceLocString.empty()) {
        return llvm::json::Object();
    }    
    size_t start = sourceLocString.find('{');
    size_t end = sourceLocString.rfind('}');
    if (start == std::string::npos || end == std::string::npos) {
        return llvm::json::Object();
    }

    std::string parsedLocString = sourceLocString.substr(start, end - start + 1);
    llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(parsedLocString);
    if (!parsed) {
        // In case of parsing error, return an empty object.
        // We can ignore the error for this utility's purpose.
        llvm::consumeError(parsed.takeError());
        return llvm::json::Object();
    }

    if (const auto* obj = parsed->getAsObject()) {
        return *obj;
    }

    return llvm::json::Object();
}

const ICFGNode* findICFGNodeByLocation(const ICFG* icfg, const std::string& location) {
    size_t colon_pos = location.find(':');
    if (colon_pos == std::string::npos) {
        return nullptr;
    }
    std::string target_filename = location.substr(0, colon_pos);
    long long target_line;
    try {
        target_line = std::stoll(location.substr(colon_pos + 1));
    } catch (const std::invalid_argument& ia) {
        return nullptr;
    }

    for (auto const& [id, node] : *icfg) {
        if (node) {
            llvm::json::Object locInfo = parseSourceLocation(node->getSourceLoc());
            if (!locInfo.empty()) {
                if (auto file = locInfo.getString("fl")) {
                    if (auto line = locInfo.getInteger("ln")) {
                        // Check if the filename contains the target filename (to handle relative/absolute paths)
                        // and if the line number matches.
                        if (file->str().find(target_filename) != std::string::npos && *line == target_line) {
                            return node;
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}

FunctionSourceInfo getFunctionSourceInfo(const llvm::Function* llvmFun) {
    if (!llvmFun) {
        return {"", 0, 0};
    }

    // Get the debug info subprogram for the function
    llvm::DISubprogram* disub = llvmFun->getSubprogram();
    if (!disub) {
        // If no debug info, we cannot determine the line numbers
        return {"", 0, 0};
    }

    std::string filename = disub->getFilename().str();
    unsigned startLine = disub->getLine();
    unsigned endLine = startLine;

    // Iterate through all instructions in the function to find the max line number
    for (const auto& bb : *llvmFun) {
        for (const auto& inst : bb) {
            const llvm::DebugLoc& loc = inst.getDebugLoc();
            if (loc && loc.getLine() > endLine) {
                endLine = loc.getLine();
            }
        }
    }
    // The end line from DISubprogram (if available and different) might be more accurate
    // as it can account for the closing brace, but iterating instructions is a robust fallback.
    return {filename, startLine, endLine};
}

void sendJsonError(const std::string& message) {
    llvm::json::Object result;
    result["error"] = true;
    result["message"] = message;
    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
}

llvm::json::Object getFunctionInfoJson(const llvm::Function* llvmFun) {
    llvm::json::Object funInfoJson;

    if (!llvmFun) {
        funInfoJson["function_name"] = "unknown";
        return funInfoJson;
    }

    FunctionSourceInfo sourceInfo = getFunctionSourceInfo(llvmFun);
    funInfoJson["function_name"] = llvmFun->getName().str();
    funInfoJson["filename"] = sourceInfo.filename;
    funInfoJson["start_line"] = sourceInfo.startLine;
    funInfoJson["end_line"] = sourceInfo.endLine;

    return funInfoJson;
}

} // namespace GraphReaderUtil
} // namespace SVF
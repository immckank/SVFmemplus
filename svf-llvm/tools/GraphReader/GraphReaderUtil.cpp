#include "GraphReaderUtil.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVFIR/SVFIR.h"
#include "SVF-LLVM/LLVMModule.h"
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

std::string getSourceVariableName(const SVFVar* var) {
    if (!var) {
        return "";
    }

    // Get the underlying llvm::Value from the SVFVar.
    // We need to get the base variable, as debug info is usually on the alloca.
    const SVFVar* baseVar = SVF::SVFUtil::isa<ObjVar>(var) ? SVF::SVFIR::getPAG()->getBaseObject(var->getId()) : SVF::SVFUtil::isa<ValVar>(var) ? SVF::SVFIR::getPAG()->getBaseValVar(var->getId()) : var;
    if (!baseVar) baseVar = var;

    const llvm::Value* llvmVal = SVF::LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(baseVar);
    if (!llvmVal) {
        return "";
    }

    // Search for llvm.dbg.declare intrinsic that refers to this value.
    if (const auto* inst = SVFUtil::dyn_cast<llvm::Instruction>(llvmVal)) {
        const auto* func = inst->getFunction();
        for (const auto& bb : *func) {
            for (const auto& i : bb) {
                if (const auto* ddi = SVFUtil::dyn_cast<llvm::DbgDeclareInst>(&i)) {
                    if (ddi->getAddress() == llvmVal) {
                        return ddi->getVariable()->getName().str();
                    }
                }
            }
        }
    }
    return ""; // Return empty if no debug info found
}

std::vector<const SVFVar*> findVarByLocation(const SVFIR* pag, const std::string& location) {
    std::vector<const SVFVar*> results;
    for (SVFIR::const_iterator it = pag->begin(), eit = pag->end(); it != eit; ++it) {
        const PAGNode* pagNode = it->second;
        if (SVFUtil::isa<ValVar>(pagNode)) {
            std::string locString = pagNode->getSourceLoc();
            llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(locString);
            if (auto file = locInfo.getString("fl")) {
                if (auto line = locInfo.getInteger("ln")) {
                    std::string formattedLoc = file->str() + ":" + std::to_string(*line);
                    if (formattedLoc == location) {
                        results.push_back(pagNode);
                        SVF::SVFUtil::outs() << "Found ValVar at " << pagNode->getSourceLoc() << "\n";
                        SVF::SVFUtil::outs() << "Var info: " << SVFUtil::cast<ValVar>(pagNode)->toString() << "\n";
                        std::string sourceName = getSourceVariableName(pagNode);
                        SVF::SVFUtil::outs() << "IR Name: " << pagNode->getName() << "\n";
                        SVF::SVFUtil::outs() << "Source Name: " << (sourceName.empty() ? "(not found)" : sourceName) << "\n";
                    }
                }
            }
        } else if (SVFUtil::isa<ObjVar>(pagNode)) {
            std::string locString = pagNode->getSourceLoc();
            llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(locString);
            if (auto file = locInfo.getString("fl")) {
                if (auto line = locInfo.getInteger("ln")) {
                    std::string formattedLoc = file->str() + ":" + std::to_string(*line);
                    if (formattedLoc == location) {
                        results.push_back(pagNode);
                        SVF::SVFUtil::outs() << "Found ObjVar at " << pagNode->getSourceLoc() << "\n";
                        SVF::SVFUtil::outs() << "Var info: " << SVFUtil::cast<ObjVar>(pagNode)->toString() << "\n";
                        std::string sourceName = getSourceVariableName(pagNode);
                        SVF::SVFUtil::outs() << "IR Name: " << pagNode->getName() << "\n";
                        SVF::SVFUtil::outs() << "Source Name: " << (sourceName.empty() ? "(not found)" : sourceName) << "\n";
                    }
                }
            }
        } else {
            // impossible
            SVF::SVFUtil::outs() << "not a var or obj" << "\n";
        }
    }
    return results;
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
#include "GraphReaderUtil.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVFIR/SVFIR.h"
#include "Graphs/SVFG.h"
#include "Graphs/SVFGNode.h"
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

    const llvm::Value* llvmVal = SVF::LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(var);
    if (!llvmVal) {
        return "";
    }

    // The llvm::Value we are looking for is often an AllocaInst, which has the debug info.
    // We may need to traverse back from the current value to find it.
    std::vector<const llvm::Value*> worklist;
    worklist.push_back(llvmVal);

    llvm::SmallPtrSet<const llvm::Value*, 8> visited;

    while (!worklist.empty()) {
        const llvm::Value* currentVal = worklist.back();
        worklist.pop_back();

        if (!visited.insert(currentVal).second) {
            continue;
        }

        // If we found an AllocaInst, check for its debug info.
        if (SVFUtil::isa<llvm::AllocaInst>(currentVal)) {
            for (const llvm::User* user : currentVal->users()) {
                if (const auto* ddi = SVFUtil::dyn_cast<llvm::DbgDeclareInst>(user)) {
                    if (ddi->getAddress() == currentVal) {
                        return ddi->getVariable()->getName().str();
                    }
                }
            }
        }

        // If the current value is an instruction, trace back its operands.
        if (const auto* inst = SVFUtil::dyn_cast<llvm::Instruction>(currentVal)) {
            // For a load, the pointer operand is what we are interested in.
            if (const auto* loadInst = SVFUtil::dyn_cast<llvm::LoadInst>(inst)) {
                worklist.push_back(loadInst->getPointerOperand());
            } else {
                // For other instructions, check all operands.
                for (const llvm::Use& op : inst->operands()) {
                    worklist.push_back(op.get());
                }
            }
        }
    }

    return ""; // Return empty if no debug info found
}

void printDebugSourceNames(const SVFVar* var) {
    if (!var) return;

    const llvm::Value* llvmVal = SVF::LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(var);
    if (!llvmVal) return;

    SVF::SVFUtil::outs() << "  Debug: Searching for related source names...\n";

    std::vector<const llvm::Value*> worklist;
    worklist.push_back(llvmVal);
    llvm::SmallPtrSet<const llvm::Value*, 16> visited;

    while (!worklist.empty()) {
        const llvm::Value* currentVal = worklist.back();
        worklist.pop_back();

        if (!visited.insert(currentVal).second) {
            continue;
        }

        // Check for debug info on the current value
        for (const llvm::User* user : currentVal->users()) {
            if (const auto* ddi = SVFUtil::dyn_cast<llvm::DbgDeclareInst>(user)) {
                if (ddi->getAddress() == currentVal) {
                    SVF::SVFUtil::outs() << "    - Found potential source name '" << ddi->getVariable()->getName().str() << "' from value: " << ddi->getAddress()->getName().str() << "\n";
                }
            }
        }

        if (const auto* inst = SVFUtil::dyn_cast<llvm::Instruction>(currentVal)) {
            for (const llvm::Use& op : inst->operands()) {
                worklist.push_back(op.get());
            }
        }
    }
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
                        if (sourceName.empty()) {
                            printDebugSourceNames(pagNode);
                        }
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
                        if (sourceName.empty()) {
                            printDebugSourceNames(pagNode);
                        }
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

std::vector<const SVFVar*> findDefinedVarByLocation(const SVFIR* pag, const SVF::SVFG* svfg, const std::string& location) {
    std::vector<const SVFVar*> results;
    for (SVFIR::const_iterator it = pag->begin(), eit = pag->end(); it != eit; ++it) {
        const PAGNode* pagNode = it->second;
        std::string locString = pagNode->getSourceLoc();
        if (locString.empty()) continue;
        llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(locString);
        if (auto file = locInfo.getString("fl")) {
            if (auto line = locInfo.getInteger("ln")) {
                std::string formattedLoc = file->str() + ":" + std::to_string(*line);
                if (formattedLoc == location) {
                    if (svfg->hasDefSVFGNode(pagNode)) {
                        SVF::SVFUtil::outs() << "Found defined pag at " << pagNode->getSourceLoc() << "\n";
                        results.push_back(pagNode); 
                        const SVFGNode* defSVFGNode = svfg->getDefSVFGNode(pagNode);
                        SVF::SVFUtil::outs() << "DefSVFGNode info: " << defSVFGNode->toString() << "\n";
                        
                    } else {
                        SVF::SVFUtil::outs() << "Not found defined pag at " << pagNode->getSourceLoc() << "\n";
                    }
                    if (SVFUtil::isa<ValVar>(pagNode)) {
                        SVF::SVFUtil::outs() << "Var info: " << SVFUtil::cast<ValVar>(pagNode)->toString() << "\n";
                    } else {
                        SVF::SVFUtil::outs() << "Var info: " << SVFUtil::cast<ObjVar>(pagNode)->toString() << "\n";
                    }
                    SVF::SVFUtil::outs() << "\n";
                }
            }
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

llvm::json::Object formatBranchInfo(const IntraCFGEdge* intraEdge) {
    std::string locString = intraEdge->getSrcNode()->getSourceLoc();
    std::string formattedLoc = "unknown";
    
    llvm::json::Object locInfo = parseSourceLocation(locString);
    if (auto file = locInfo.getString("fl")) {
        if (auto line = locInfo.getInteger("ln")) {
            formattedLoc = file->str() + ":" + std::to_string(*line);
        }
    }
    return llvm::json::Object{
        {"type", "branch"},
        {"location", formattedLoc},
        {"condition_value", intraEdge->getSuccessorCondValue() == 1 ? "true" : "false"}
    };
}

} // namespace GraphReaderUtil
} // namespace SVF
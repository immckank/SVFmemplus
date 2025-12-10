#include "GraphReaderUtil.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVFIR/SVFIR.h"
#include "Graphs/SVFG.h"
#include "Graphs/SVFGNode.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include "MemoryModel/PointerAnalysis.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Operator.h"
#include <limits>
#include <map>
#include <queue>
#include <unordered_set>

namespace SVF {
namespace GraphReaderUtil {

namespace {

static std::string resolveCalleeName(const llvm::CallBase* callInst) {
    if (!callInst) {
        return "";
    }

    if (const llvm::Function* directCallee = callInst->getCalledFunction()) {
        return directCallee->getName().str();
    }

    const llvm::Value* calledOperand = callInst->getCalledOperand();
    if (calledOperand && calledOperand->hasName()) {
        return calledOperand->getName().str();
    }

    return "";
}

static const CallICFGNode* selectCallICFGNode(ICFG* icfg, const std::string& location,  const std::string& functionName) {
    // TODO: 
    // 这个函数依旧有缺陷 如果这个一行处多次调用了同名函数应该怎么处理
    std::vector<const ICFGNode*> allNodes = findAllICFGNodesByLocation(icfg, location);
    if (allNodes.empty()) {
        return nullptr;
    }

    struct CandidateInfo {
        const CallICFGNode* callNode;
        std::string calleeName;
        std::string sourceLoc;
    };

    std::vector<CandidateInfo> candidates;
    for (const ICFGNode* node : allNodes) {
        if (const auto* callNode = SVFUtil::dyn_cast<CallICFGNode>(node)) {
            const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(callNode);
            const llvm::CallBase* callInst = SVFUtil::dyn_cast<llvm::CallBase>(llvmVal);
            std::string calleeName = resolveCalleeName(callInst);
            candidates.push_back({callNode, calleeName, callNode->getSourceLoc()});
        }
    }

    if (candidates.empty()) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] No CallICFGNode found at location '"
                             << location << "'\n";
        return nullptr;
    }

    const CallICFGNode* matchedNode = nullptr;
    for (const auto& candidate : candidates) {
        if (!candidate.calleeName.empty() && candidate.calleeName == functionName) {
            matchedNode = candidate.callNode;
            break;
        }
    }

    if (!matchedNode) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Could not find call to '" << functionName
                             << "' at location '" << location << "'. Available callees:";
        for (const auto& candidate : candidates) {
            SVF::SVFUtil::errs() << " '"
                                 << (candidate.calleeName.empty() ? std::string("<unknown>") : candidate.calleeName)
                                 << "'";
        }
        SVF::SVFUtil::errs() << "\n";
        return nullptr;
    }

    return matchedNode;
}

static std::string toString(const llvm::Type* type) {
    if (!type) {
        return "";
    }
    std::string result;
    llvm::raw_string_ostream rso(result);
    type->print(rso);
    return rso.str();
}

static std::string toString(const llvm::Value* value) {
    if (!value) {
        return "";
    }
    std::string result;
    llvm::raw_string_ostream rso(result);
    value->print(rso, false);
    return rso.str();
}

static const llvm::Module* getAnyModule() {
    LLVMModuleSet* llvms = LLVMModuleSet::getLLVMModuleSet();
    if (!llvms) {
        return nullptr;
    }
    const auto& modules = llvms->getLLVMModules();
    if (modules.empty()) {
        return nullptr;
    }
    return &modules.front().get();
}

static bool isFunctionArgument(const llvm::Value* value) {
    if (!value) {
        return false;
    }
    value = value->stripPointerCasts();
    return llvm::isa<llvm::Argument>(value);
}

static bool reachesFormalParameter(SVFG* svfg, const SVFVar* startVar, unsigned maxDepth = 256) {
    if (!svfg || !startVar) {
        return false;
    }

    if (!svfg->hasDefSVFGNode(startVar)) {
        return false;
    }

    std::queue<std::pair<const SVFGNode*, unsigned>> worklist;
    std::unordered_set<const SVFGNode*> visited;

    worklist.emplace(svfg->getDefSVFGNode(startVar), 0);

    while (!worklist.empty()) {
        auto [node, depth] = worklist.front();
        worklist.pop();

        if (!node) {
            continue;
        }

        if (!visited.insert(node).second) {
            continue;
        }

        if (SVFUtil::isa<FormalParmVFGNode>(node)) {
            return true;
        }

        if (depth >= maxDepth) {
            continue;
        }

        for (auto it = node->InEdgeBegin(); it != node->InEdgeEnd(); ++it) {
            const SVFGEdge* edge = *it;
            if (!edge) {
                continue;
            }
            const SVFGNode* srcNode = edge->getSrcNode();
            if (srcNode) {
                worklist.emplace(srcNode, depth + 1);
            }
        }
    }

    return false;
}

} // anonymous namespace

bool parseCommandsLine(const std::string& jsonStr,
                       std::vector<llvm::json::Object>& outCmds,
                       std::string& errMsg) {
    outCmds.clear();

    auto parsed = llvm::json::parse(jsonStr);
    if (!parsed) {
        errMsg = llvm::toString(parsed.takeError());
        return false;
    }

    llvm::json::Value root = std::move(*parsed);

    if (auto* arr = root.getAsArray()) {
        for (auto& v : *arr) {
            if (auto* o = v.getAsObject()) {
                outCmds.push_back(*o);
            }
        }
        return true;
    }

    if (auto* obj = root.getAsObject()) {
        if (auto* arr2 = obj->getArray("commands")) {
            for (auto& v : *arr2) {
                if (auto* o = v.getAsObject()) {
                    outCmds.push_back(*o);
                }
            }
            return true;
        }
        outCmds.push_back(*obj);
        return true;
    }

    errMsg = "unsupported json format";
    return false;
}

void sendJsonError(const std::string& message) {
    llvm::json::Object result;
    result["error"] = true;
    result["message"] = message;
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
    llvm::outs().flush();
}

std::string getSVFGNodeKindString(const SVFGNode* node, bool detailed) {
    // DEBUG
    // 基本debug功能保留
    if (!node) {
        return "Null";
    }

    auto pickName = [detailed](llvm::StringRef simple, llvm::StringRef detail) -> std::string {
        return detailed ? detail.str() : simple.str();
    };

    if (SVFUtil::isa<StoreVFGNode>(node)) {
        return pickName("Store", "StoreVFGNode");
    }
    if (SVFUtil::isa<LoadVFGNode>(node)) {
        return pickName("Load", "LoadVFGNode");
    }
    if (SVFUtil::isa<CopyVFGNode>(node)) {
        return pickName("Copy", "CopyVFGNode");
    }
    if (SVFUtil::isa<GepVFGNode>(node)) {
        return pickName("Gep", "GepVFGNode");
    }
    if (SVFUtil::isa<AddrVFGNode>(node)) {
        return pickName("Addr", "AddrVFGNode");
    }
    if (SVFUtil::isa<PHIVFGNode>(node)) {
        return pickName("TPhi", "PHIVFGNode");
    }
    if (SVFUtil::isa<FormalParmVFGNode>(node)) {
        return pickName("FParm", "FormalParmVFGNode");
    }
    if (SVFUtil::isa<ActualParmVFGNode>(node)) {
        return pickName("AParm", "ActualParmVFGNode");
    }
    if (SVFUtil::isa<FormalRetVFGNode>(node)) {
        return pickName("FRet", "FormalRetVFGNode");
    }
    if (SVFUtil::isa<ActualRetVFGNode>(node)) {
        return pickName("ARet", "ActualRetVFGNode");
    }
    if (SVFUtil::isa<MRSVFGNode>(node)) {
        return "MRSVFGNode";
    }

    switch (node->getNodeKind()) {
        case SVFValue::Addr: return "Addr";
        case SVFValue::Copy: return "Copy";
        case SVFValue::Gep: return "Gep";
        case SVFValue::Store: return "Store";
        case SVFValue::Load: return "Load";
        case SVFValue::Cmp: return "Cmp";
        case SVFValue::BinaryOp: return "BinaryOp";
        case SVFValue::UnaryOp: return "UnaryOp";
        case SVFValue::Branch: return "Branch";
        case SVFValue::DummyVProp: return "DummyVProp";
        case SVFValue::NPtr: return "NPtr";
        case SVFValue::FRet: return "FRet";
        case SVFValue::ARet: return "ARet";
        case SVFValue::AParm: return "AParm";
        case SVFValue::FParm: return "FParm";
        case SVFValue::TPhi: return "TPhi";
        case SVFValue::TIntraPhi: return "TIntraPhi";
        case SVFValue::TInterPhi: return "TInterPhi";
        case SVFValue::FPIN: return "FPIN";
        case SVFValue::FPOUT: return "FPOUT";
        case SVFValue::APIN: return "APIN";
        case SVFValue::APOUT: return "APOUT";
        case SVFValue::MPhi: return "MPhi";
        case SVFValue::MIntraPhi: return "MIntraPhi";
        case SVFValue::MInterPhi: return "MInterPhi";
        default: return detailed ? "VFGNode" : "Unknown";
    }
}

llvm::json::Object parseSourceLocation(const std::string& sourceLocString) {
    if (sourceLocString.empty()) {
        return llvm::json::Object();
    }
    
    // Strategy: Try to find the JSON object containing location fields ("ln", "cl", "fl")
    // We search from the end of the string backwards for valid JSON objects
    // This is necessary because some node types (e.g., IntraMSSAPHISVFGNode) have multiple
    // {...} patterns in their toString() output, and the location info is typically at the end.
    // Example: "IntraMSSAPHISVFGNode ID: 116550 {fun: ...}...pts{3742 }{ "ln": 1053, "cl": 5, "fl": "tif_dirwrite.c" }"
    
    // First, try to find a JSON-like pattern with quotes (location info format)
    // Location info typically appears as: { "ln": 1053, "cl": 5, "fl": "tif_dirwrite.c" }
    size_t lastBrace = sourceLocString.rfind('}');
    
    while (lastBrace != std::string::npos) {
        // Find the corresponding opening brace by searching backwards
        int braceCount = 1;
        size_t pos = lastBrace;
        
        while (pos > 0 && braceCount > 0) {
            pos--;
            if (sourceLocString[pos] == '}') {
                braceCount++;
            } else if (sourceLocString[pos] == '{') {
                braceCount--;
            }
        }
        
        if (braceCount == 0) {
            // Found a matching pair of braces
            std::string candidate = sourceLocString.substr(pos, lastBrace - pos + 1);
            
            // Try to parse this candidate
            llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(candidate);
            if (parsed) {
                if (const auto* obj = parsed->getAsObject()) {
                    // Check if this object contains location fields
                    if (obj->getString("fl") || obj->getInteger("ln")) {
                        return *obj;
                    }
                }
            } else {
                // Consume the error
                llvm::consumeError(parsed.takeError());
            }
        }
        
        // Try to find the next (previous) closing brace
        if (pos > 0) {
            lastBrace = sourceLocString.rfind('}', pos - 1);
        } else {
            break;
        }
    }
    
    // Fallback: try the old method (first { to last })
    size_t start = sourceLocString.find('{');
    size_t end = sourceLocString.rfind('}');
    if (start != std::string::npos && end != std::string::npos && start < end) {
        std::string parsedLocString = sourceLocString.substr(start, end - start + 1);
        llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(parsedLocString);
        if (parsed) {
            if (const auto* obj = parsed->getAsObject()) {
                return *obj;
            }
        } else {
            llvm::consumeError(parsed.takeError());
        }
    }

    return llvm::json::Object();
}

llvm::json::Object getFunctionInfoJson(const llvm::Function* llvmFun) {
    llvm::json::Object funInfoJson;

    if (!llvmFun) {
        funInfoJson["function_name"] = "unknown";
        funInfoJson["filename"] = "";
        funInfoJson["start_line"] = 0;
        funInfoJson["end_line"] = 0;
        return funInfoJson;
    }

    funInfoJson["function_name"] = llvmFun->getName().str();

    // Get the debug info subprogram for the function
    llvm::DISubprogram* disub = llvmFun->getSubprogram();
    if (!disub) {
        // If no debug info, we cannot determine the line numbers
        funInfoJson["filename"] = "";
        funInfoJson["start_line"] = 0;
        funInfoJson["end_line"] = 0;
        return funInfoJson;
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

    funInfoJson["filename"] = filename;
    funInfoJson["start_line"] = startLine;
    funInfoJson["end_line"] = endLine;

    return funInfoJson;
}

llvm::json::Object getStoreClInfoJson(SVFG* svfg, ICFG* icfg, const std::string& location) {
    llvm::json::Object result;
    result["location"] = location;
    llvm::json::Array storeCls;
    
    // 使用findAllICFGNodesByLocation找到所有匹配的ICFG节点
    std::vector<const ICFGNode*> icfgNodes = findAllICFGNodesByLocation(icfg, location);
    
    if (icfgNodes.empty()) {
        result["store_cl"] = std::move(storeCls);
        return result;
    }
    
    // 对于每个ICFG节点，查找对应的SVFG节点
    for (const ICFGNode* icfgNode : icfgNodes) {
        // 遍历SVFG的所有节点，找到与当前ICFG节点关联的节点
        for (auto& pair : *svfg) {
            SVFGNode* svfgNode = pair.second;
            if (svfgNode->getICFGNode() == icfgNode) {
                // 检查是否是StoreVFGNode
                if (SVFUtil::isa<StoreVFGNode>(svfgNode)) {
                    // 获取该节点的源位置信息
                    std::string sourceLocStr = svfgNode->toString();
                    llvm::json::Object locInfo = parseSourceLocation(sourceLocStr);
                    
                    // 获取cl（列号）信息
                    if (auto cl = locInfo.getInteger("cl")) {
                        storeCls.push_back(*cl);
                    }
                }
            }
        }
    }
    
    result["store_cl"] = std::move(storeCls);
    return result;
}

llvm::json::Object getGepClInfoJson(SVFG* svfg, ICFG* icfg, const std::string& location) {
    llvm::json::Object result;
    result["location"] = location;
    llvm::json::Array gepCls;
    llvm::json::Array gepClNodes;
    std::map<int64_t, llvm::json::Array> clToNodes;
    
    std::vector<const ICFGNode*> icfgNodes = findAllICFGNodesByLocation(icfg, location);
    if (icfgNodes.empty()) {
        result["gep_cl"] = std::move(gepCls);
        result["gep_cl_nodes"] = std::move(gepClNodes);
        return result;
    }
    
    for (const ICFGNode* icfgNode : icfgNodes) {
        // 遍历SVFG的所有节点，找到与当前ICFG节点关联的节点
        for (auto& pair : *svfg) {
            SVFGNode* svfgNode = pair.second;
            if (svfgNode->getICFGNode() == icfgNode) {
                // 检查是否是GepVFGNode
                if (SVFUtil::isa<GepVFGNode>(svfgNode)) {
                    // 获取该节点的源位置信息
                    std::string sourceLocStr = svfgNode->toString();
                    llvm::json::Object locInfo = parseSourceLocation(sourceLocStr);
                    
                    // 获取cl（列号）信息
                    if (auto cl = locInfo.getInteger("cl")) {
                        gepCls.push_back(*cl);
                        clToNodes[*cl].push_back(sourceLocStr);
                    }
                }
            }
        }
    }
    
    result["gep_cl"] = std::move(gepCls);
    for (auto& entry : clToNodes) {
        llvm::json::Object clInfo;
        clInfo["cl"] = entry.first;
        clInfo["svfg_nodes"] = std::move(entry.second);
        gepClNodes.push_back(std::move(clInfo));
    }
    result["gep_cl_nodes"] = std::move(gepClNodes);
    return result;
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

std::vector<const ICFGNode*> findAllICFGNodesByLocation(const ICFG* icfg, const std::string& location) {
    std::vector<const ICFGNode*> results;
    size_t colon_pos = location.find(':');
    if (colon_pos == std::string::npos) {
        return results;
    }
    std::string target_filename = location.substr(0, colon_pos);
    long long target_line;
    try {
        target_line = std::stoll(location.substr(colon_pos + 1));
    } catch (const std::invalid_argument& ia) {
        return results;
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
                            results.push_back(node);
                        }
                    }
                }
            }
        }
    }
    return results;
}

const PAGNode* getPAGNodeFromArg(SVFIR* pag, const std::string& funcName, int argIndex) {
    if (!pag) {
        return nullptr;
    }
    const FunObjVar* fun = pag->getFunObjVar(funcName);
    if (!fun) {
        return nullptr;
    }
    const SVFIR::SVFVarList& args = pag->getFunArgsList(fun);
    if (argIndex < 0 || static_cast<size_t>(argIndex) >= args.size()) {
        return nullptr;
    }
    const SVFVar* arg = args[argIndex];
    if (!arg) {
        return nullptr;
    }
    return pag->getGNode(arg->getId());
}

const PAGNode* getPAGNodeFromLvar(ICFG* icfg, SVFIR* pag, const std::string& location, int eqPosition) {
    if (!icfg || !pag) {
        return nullptr;
    }
    
    std::vector<const ICFGNode*> allICFGNodes = findAllICFGNodesByLocation(icfg, location);
    if (allICFGNodes.empty()) {
        return nullptr;
    }
    
    int idx = 0;
    for (size_t i = 0; i < allICFGNodes.size(); i++) {
        const ICFGNode* icfgNode = allICFGNodes[i];
        if (SVFUtil::isa<IntraICFGNode>(icfgNode)) {
            const std::string sourceLocation = icfgNode->getSourceLoc();
            llvm::json::Object locObj = parseSourceLocation(sourceLocation);
            if (locObj.empty()) {
                continue;
            }
            if (auto cl = locObj.getInteger("cl")) {
                if (*cl == eqPosition) {
                    idx = i;
                    break;
                }
            }
        }
    }
    
    const ICFGNode* targetICFGNode = allICFGNodes[idx];
    auto intraNode = SVFUtil::dyn_cast<IntraICFGNode>(targetICFGNode);
    if (!intraNode) {
        return nullptr;
    }
    
    if (targetICFGNode->getSVFStmts().empty()) {
        return nullptr;
    }
    
    const SVFStmt* targetStmt = targetICFGNode->getSVFStmts().front();
    const SVFVar* lhs = nullptr;
    
    if (const AddrStmt* addr = SVFUtil::dyn_cast<AddrStmt>(targetStmt)) {
        lhs = addr->getLHSVar();
    } else if (const CopyStmt* copy = SVFUtil::dyn_cast<CopyStmt>(targetStmt)) {
        lhs = copy->getLHSVar();
    } else if (const LoadStmt* load = SVFUtil::dyn_cast<LoadStmt>(targetStmt)) {
        lhs = load->getLHSVar();
    } else if (const StoreStmt* store = SVFUtil::dyn_cast<StoreStmt>(targetStmt)) {
        lhs = store->getLHSVar();
    } else if (const GepStmt* gep = SVFUtil::dyn_cast<GepStmt>(targetStmt)) {
        lhs = gep->getLHSVar();
    }
    
    return lhs;
}

// 这个函数可以用来构造gep栈
const PAGNode* getPAGNodeFromLvarGEP(ICFG* icfg, SVFIR* pag, const std::string& location, int eqPosition) {
    if (!icfg || !pag) {
        // actually impossible
        SVF::SVFUtil::errs() << "Error: Invalid ICFG or PAG pointer\n";
        return nullptr;
    }
    
    // Step 1: 根据location找到所有icfg节点
    std::vector<const ICFGNode*> allICFGNodes = findAllICFGNodesByLocation(icfg, location);
    if (allICFGNodes.empty()) {
        SVF::SVFUtil::errs() << "Error: No ICFG nodes found at location: " << location << "\n";
        return nullptr;
    }
    
    // Step 2: 找到在赋值操作左侧的icfg节点，根据eqPosition筛选，并找到Store语句的LHS变量
    const SVFVar* storeLHS = nullptr;
    
    for (size_t i = 0; i < allICFGNodes.size(); i++) {
        const ICFGNode* icfgNode = allICFGNodes[i];
        
        if (SVFUtil::isa<IntraICFGNode>(icfgNode)) {
            const std::string sourceLocation = icfgNode->getSourceLoc();
            
            llvm::json::Object locObj = parseSourceLocation(sourceLocation);
            if (locObj.empty()) {
                continue;
            }
            if (auto cl = locObj.getInteger("cl")) {
                if (*cl == eqPosition) {
                    // 查找Store语句并获取LHS
                    for (const SVFStmt* stmt : icfgNode->getSVFStmts()) {
                        if (const StoreStmt* store = SVFUtil::dyn_cast<StoreStmt>(stmt)) {
                            storeLHS = store->getLHSVar();
                            break;
                        }
                    }
                    break;
                }
            }
        }
    }
    
    if (!storeLHS) {
        SVF::SVFUtil::errs() << "Error: Could not find Store statement at eq_position " << eqPosition << "\n";
        return nullptr;
    }
    
    // Step 3: 在所有列号<=eqPosition的节点中查找GEP语句，其LHS与Store的LHS匹配
    const SVFVar* baseVar = nullptr;
    
    for (size_t i = 0; i < allICFGNodes.size(); i++) {
        const ICFGNode* icfgNode = allICFGNodes[i];
        
        // 检查列号是否<=eqPosition
        if (SVFUtil::isa<IntraICFGNode>(icfgNode)) {
            const std::string sourceLocation = icfgNode->getSourceLoc();
            llvm::json::Object locObj = parseSourceLocation(sourceLocation);
            if (!locObj.empty()) {
                if (auto cl = locObj.getInteger("cl")) {
                    if (*cl > eqPosition) {
                        continue;
                    }
                }
            }
        }
        
        // 遍历该节点的所有语句
        for (const SVFStmt* stmt : icfgNode->getSVFStmts()) {
            if (const GepStmt* gep = SVFUtil::dyn_cast<GepStmt>(stmt)) {
                const SVFVar* gepLHS = gep->getLHSVar();
                
                // 检查GEP的LHS是否与Store的LHS匹配
                if (gepLHS->getId() == storeLHS->getId()) {
                    baseVar = gep->getRHSVar();
                    break;
                }
            }
        }
        
        if (baseVar) break;
    }
    
    if (!baseVar) {
        baseVar = storeLHS;
    }
    
    // Step 4: 如果获取到的是GepValVar，递归获取其最基础的base对象
    while (baseVar && SVFUtil::isa<GepValVar>(baseVar)) {
        const GepValVar* gepValVar = SVFUtil::dyn_cast<GepValVar>(baseVar);
        baseVar = gepValVar->getBaseNode();
    }
    
    if (!baseVar) {
        SVF::SVFUtil::errs() << "Error: Could not determine base variable\n";
    }
    
    return baseVar;
}

// direct pag node to call arg
const PAGNode* getPAGNodeFromCallArg(ICFG* icfg, SVFIR* pag, const std::string& location, int argIndex, const std::string& functionName) {
    if (!icfg || !pag) {
        SVF::SVFUtil::errs() << "Error: Invalid ICFG or PAG pointer\n";
        return nullptr;
    }

    const CallICFGNode* callNode = selectCallICFGNode(icfg, location, functionName);
    if (!callNode) {
        SVF::SVFUtil::errs() << "Error: No CallICFGNode found at location: " << location << "\n";
        return nullptr;
    }

    const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(callNode);
    const llvm::CallBase* callInst = SVFUtil::dyn_cast<llvm::CallBase>(llvmVal);
    if (!callInst) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Failed to obtain CallBase from CallICFGNode at "
                             << location << "\n";
        return nullptr;
    }

    if (argIndex < 0 || argIndex >= static_cast<int>(callInst->arg_size())) {
        SVF::SVFUtil::errs() << "Error: Argument index " << argIndex
                             << " out of range for call at " << location
                             << " (argument count=" << callInst->arg_size() << ")\n";
        return nullptr;
    }

    const llvm::Value* targetArgVal = callInst->getArgOperand(argIndex);
    if (!targetArgVal) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Call argument operand is null at index "
                             << argIndex << " for location '" << location << "'\n";
        return nullptr;
    }

    NodeID argNodeID = LLVMModuleSet::getLLVMModuleSet()->getValueNode(targetArgVal);
    if (argNodeID == UINT_MAX || !pag->hasGNode(argNodeID)) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Could not find PAG node for argument index "
                             << argIndex << " at location '" << location << "'\n";
        return nullptr;
    }

    const PAGNode* targetNode = pag->getGNode(argNodeID);
    return targetNode;
}

// store pag node to call arg
const PAGNode* tracePAGNodeFromCallArg(SVFG* svfg, ICFG* icfg, SVFIR* pag, const std::string& callLocation, const std::string& functionName, int argIndex) {
    // 这个与前面那个的区别是 他会先找callicfg节点 然后找对应的paramsvfg节点 最后沿着值流图找到定义位置的pag
    SVF::SVFUtil::outs() << "[GraphReaderUtil] tracePAGNodeFromCallArg start. Location='"
                         << callLocation << "', function filter='"
                         << (functionName.empty() ? std::string("<none>") : functionName)
                         << "', arg_index=" << argIndex << "\n";

    const CallICFGNode* callNode = selectCallICFGNode(icfg, callLocation, functionName);
    if (!callNode) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Unable to resolve call node for tracePAGNodeFromCallArg.\n";
        return nullptr;
    }

    // Step 2: Find ActualParmVFGNode for the specified argument
    std::vector<const ActualParmVFGNode*> actualParmNodes;
    for (auto& pair : *svfg) {
        SVFGNode* svfgNode = pair.second;
        if (svfgNode->getICFGNode() == callNode) {
            if (auto apNode = SVFUtil::dyn_cast<ActualParmVFGNode>(svfgNode)) {
                actualParmNodes.push_back(apNode);
            }
        }
    }

    SVF::SVFUtil::outs() << "[GraphReaderUtil] Found " << actualParmNodes.size()
                         << " ActualParmVFGNode candidate(s) at this call site.\n";

    if (actualParmNodes.empty()) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] No ActualParmVFGNode found for this call.\n";
        return nullptr;
    }

    const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(callNode);
    const llvm::CallBase* callInst = SVFUtil::dyn_cast<llvm::CallBase>(llvmVal);

    if (!callInst) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Could not obtain CallBase from CallICFGNode.\n";
        return nullptr;
    }

    SVF::SVFUtil::outs() << "[GraphReaderUtil] Call has " << callInst->arg_size() << " argument(s).\n";

    if (argIndex < 0 || argIndex >= static_cast<int>(callInst->arg_size())) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Argument index " << argIndex
                             << " is invalid for call at '" << callLocation << "'.\n";
        return nullptr;
    }

    const llvm::Value* targetArgVal = callInst->getArgOperand(argIndex);
    if (!targetArgVal) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Target argument value is null at index "
                             << argIndex << ".\n";
        return nullptr;
    }

    // Find the ActualParmVFGNode that corresponds to this argument
    const ActualParmVFGNode* targetParam = nullptr;
    for (const ActualParmVFGNode* apNode : actualParmNodes) {
        const llvm::Value* paramLLVMVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(apNode->getParam());
        if (paramLLVMVal == targetArgVal) {
            targetParam = apNode;
            break;
        }
    }

    if (!targetParam) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Could not map LLVM argument to ActualParmVFGNode.\n";
        return nullptr;
    }

    SVF::SVFUtil::outs() << "[GraphReaderUtil] Matched ActualParmVFGNode ID=" << targetParam->getId()
                         << " for argument index " << argIndex << "\n";

    // Step 3: Get the PAGNode from the ActualParmVFGNode
    const SVFVar* paramPAG = targetParam->getParam();
    if (!paramPAG) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Actual parameter node has null PAG reference.\n";
        return nullptr;
    }

    const llvm::Value* paramLLVMVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(paramPAG);
    if (!paramLLVMVal) {
        SVF::SVFUtil::outs() << "[GraphReaderUtil] Actual parameter is not backed by an LLVM value; returning param PAG node.\n";
        return paramPAG;
    }

    const llvm::LoadInst* loadInst = SVFUtil::dyn_cast<llvm::LoadInst>(paramLLVMVal);
    if (!loadInst) {
        SVF::SVFUtil::outs() << "[GraphReaderUtil] Argument is not a load instruction; returning parameter PAG node.\n";
        return paramPAG;
    }

    const llvm::Value* ptrOperand = loadInst->getPointerOperand();
    NodeID ptrNodeID = LLVMModuleSet::getLLVMModuleSet()->getValueNode(ptrOperand);

    if (ptrNodeID == UINT_MAX || !pag->hasGNode(ptrNodeID)) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Could not find PAG node for pointer operand of load instruction.\n";
        return nullptr;
    }

    const SVFVar* addressPAG = pag->getGNode(ptrNodeID);
    SVF::SVFUtil::outs() << "[GraphReaderUtil] Tracing memory definition for address PAGNode ID="
                         << addressPAG->getId() << "\n";

    const SVFGNode* storeNode = nullptr;

    if (svfg && addressPAG) {
        // Strategy 1: Use SVFG backward traversal from load to find store via def-use edges
        const LoadVFGNode* loadNode = nullptr;

        for (auto it = svfg->begin(); it != svfg->end(); ++it) {
            const SVFGNode* node = it->second;
            if (const auto* load = SVFUtil::dyn_cast<LoadVFGNode>(node)) {
                const SVFStmt* pagEdge = load->getPAGEdge();
                const LoadStmt* loadStmt = SVFUtil::dyn_cast<LoadStmt>(pagEdge);
                if (loadStmt && loadStmt->getRHSVar()->getId() == addressPAG->getId()) {
                    loadNode = load;
                    break;
                }
            }
        }

        if (loadNode) {
            for (auto it = loadNode->InEdgeBegin(); it != loadNode->InEdgeEnd(); ++it) {
                const SVFGEdge* edge = *it;
                const SVFGNode* srcNode = edge->getSrcNode();

                if (const auto* candidateStore = SVFUtil::dyn_cast<StoreVFGNode>(srcNode)) {
                    storeNode = candidateStore;
                    break;
                }

                if (const auto* mssaNode = SVFUtil::dyn_cast<MRSVFGNode>(srcNode)) {
                    for (auto it2 = mssaNode->InEdgeBegin(); it2 != mssaNode->InEdgeEnd(); ++it2) {
                        const SVFGEdge* edge2 = *it2;
                        const SVFGNode* srcNode2 = edge2->getSrcNode();
                        if (const auto* candidateStore = SVFUtil::dyn_cast<StoreVFGNode>(srcNode2)) {
                            storeNode = candidateStore;
                            break;
                        }
                    }
                }

                if (storeNode) {
                    break;
                }
            }
        }

        if (!storeNode) {
            const auto* addressGep = SVFUtil::dyn_cast<GepValVar>(addressPAG);
            if (addressGep) {
                for (auto it = svfg->begin(); it != svfg->end(); ++it) {
                    const SVFGNode* node = it->second;
                    if (const auto* candidateStore = SVFUtil::dyn_cast<StoreVFGNode>(node)) {
                        const SVFStmt* pagEdge = candidateStore->getPAGEdge();
                        const StoreStmt* storeStmt = SVFUtil::dyn_cast<StoreStmt>(pagEdge);
                        if (!storeStmt) {
                            continue;
                        }

                        const SVFVar* storeLHS = storeStmt->getLHSVar();
                        if (const auto* storeLHSGep = SVFUtil::dyn_cast<GepValVar>(storeLHS)) {
                            if (storeLHSGep->getConstantFieldIdx() == addressGep->getConstantFieldIdx()) {
                                storeNode = candidateStore;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (!storeNode) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Warning: Could not locate store definition for address PAG node." << "\n";
        return nullptr;
    }

    const PAGNode* storedValuePAG = nullptr;

    if (const auto* storeVFGNode = SVFUtil::dyn_cast<StoreVFGNode>(storeNode)) {
        const SVFStmt* pagEdge = storeVFGNode->getPAGEdge();
        const StoreStmt* storeStmt = SVFUtil::dyn_cast<StoreStmt>(pagEdge);
        if (storeStmt) {
            storedValuePAG = storeStmt->getRHSVar();
        }
    }

    if (storedValuePAG) {
        SVF::SVFUtil::outs() << "[GraphReaderUtil] Located stored value PAGNode ID="
                             << storedValuePAG->getId() << " as definition source.\n";
    } else {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Warning: Store definition found but RHS PAG node is null.\n";
    }

    return storedValuePAG;
}

llvm::json::Object analyzeStoreLValue(SVFG* svfg,
                                      ICFG* icfg,
                                      SVFIR* pag,
                                      const std::string& location,
                                      int eqPosition) {
    llvm::json::Object result;
    result["command"] = "analysis-lvar";
    result["location"] = location;
    result["eq_position"] = eqPosition;

    if (!icfg || !pag) {
        result["success"] = false;
        result["error"] = "Invalid ICFG or PAG pointer";
        return result;
    }

    const llvm::Module* module = getAnyModule();
    const llvm::DataLayout* dataLayout = module ? &module->getDataLayout() : nullptr;

    std::vector<const ICFGNode*> nodes = findAllICFGNodesByLocation(icfg, location);
    if (nodes.empty()) {
        result["success"] = false;
        result["error"] = "No ICFG nodes found at location";
        return result;
    }

    const ICFGNode* matchedNode = nullptr;
    for (const ICFGNode* node : nodes) {
        if (!SVFUtil::isa<IntraICFGNode>(node)) {
            continue;
        }
        llvm::json::Object locInfo = parseSourceLocation(node->getSourceLoc());
        if (auto col = locInfo.getInteger("cl")) {
            if (*col == eqPosition) {
                matchedNode = node;
                break;
            }
        }
    }
    if (!matchedNode) {
        matchedNode = nodes.front();
    }

    result["icfg_node_id"] = static_cast<int64_t>(matchedNode->getId());

    const IntraICFGNode* intraNode = SVFUtil::dyn_cast<IntraICFGNode>(matchedNode);
    if (!intraNode) {
        result["success"] = false;
        result["error"] = "Matched node is not an IntraICFGNode";
        return result;
    }

    const StoreStmt* storeStmt = nullptr;
    for (const SVFStmt* stmt : intraNode->getSVFStmts()) {
        if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(stmt)) {
            storeStmt = store;
            break;
        }
    }

    if (!storeStmt) {
        result["success"] = false;
        result["error"] = "No StoreStmt found at specified location/column";
        return result;
    }

    result["lhs_pag_id"] = static_cast<int64_t>(storeStmt->getLHSVar()->getId());

    const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(intraNode);
    const llvm::Instruction* inst = SVFUtil::dyn_cast<llvm::Instruction>(llvmVal);
    const llvm::StoreInst* storeInst = llvm::dyn_cast<llvm::StoreInst>(inst);
    if (!storeInst) {
        result["success"] = false;
        result["error"] = "Matched instruction is not a StoreInst";
        return result;
    }

    result["success"] = true;
    result["store_ir"] = toString(storeInst);

    const llvm::Value* pointerOperand = storeInst->getPointerOperand();
    const llvm::Value* basePointerValue = pointerOperand;
    bool isStructLValue = false;
    bool isMember = false;
    std::string structName;
    int64_t memberOffset = 0;
    bool hasOffset = false;

    const llvm::Value* pointerStripped = pointerOperand ? pointerOperand->stripPointerCasts() : nullptr;
    const llvm::GEPOperator* gepOp = nullptr;
    if (pointerStripped) {
        gepOp = llvm::dyn_cast<llvm::GEPOperator>(pointerStripped);
    }
    if (!gepOp && pointerOperand) {
        gepOp = llvm::dyn_cast<llvm::GEPOperator>(pointerOperand);
    }

    if (gepOp) {
        basePointerValue = gepOp->getPointerOperand();

        const llvm::Type* sourceType = gepOp->getSourceElementType();
        if (sourceType && sourceType->isStructTy()) {
            isStructLValue = true;
            if (const auto* structTy = llvm::cast<llvm::StructType>(sourceType)) {
                if (structTy->hasName()) {
                    structName = structTy->getName().str();
                }
            }
        }
        isMember = gepOp->getNumIndices() > 1;

        if (dataLayout) {
            unsigned addrSpace = gepOp->getPointerAddressSpace();
            llvm::APInt offsetAP(dataLayout->getPointerSizeInBits(addrSpace), 0);
            if (gepOp->accumulateConstantOffset(*dataLayout, offsetAP)) {
                memberOffset = offsetAP.getSExtValue();
                hasOffset = true;
            }
        }
    } else if (pointerOperand) {
        if (const auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(pointerOperand->getType())) {
            if (!ptrTy->isOpaquePointerTy()) {
                const llvm::Type* elementType = ptrTy->getNonOpaquePointerElementType();
                if (elementType && elementType->isStructTy()) {
                    isStructLValue = true;
                    if (const auto* structTy = llvm::cast<llvm::StructType>(elementType)) {
                        if (structTy->hasName()) {
                            structName = structTy->getName().str();
                        }
                    }
                }
            }
        }
        hasOffset = true;
        memberOffset = 0;
    }

    const llvm::Value* baseStripped = basePointerValue ? basePointerValue->stripPointerCasts() : nullptr;
    bool pointerIsParam = isFunctionArgument(pointerOperand) || isFunctionArgument(pointerStripped);
    bool baseIsParam = isFunctionArgument(basePointerValue) || isFunctionArgument(baseStripped);

    if (!baseIsParam) {
        const PAGNode* basePagNode = getPAGNodeFromLvarGEP(icfg, pag, location, eqPosition);
        if (basePagNode) {
            if (const auto* baseSVFVar = SVFUtil::dyn_cast<SVFVar>(basePagNode)) {
                if (reachesFormalParameter(svfg, baseSVFVar)) {
                    baseIsParam = true;
                }
            }
        }
    }

    llvm::json::Object gepInfo;
    std::string gepType = "not_struct";
    if (isStructLValue) {
        gepType = isMember ? "member" : "baseobj";
    }
    gepInfo["gep_type"] = gepType;

    int64_t gepCl = -1;
    if (const llvm::Instruction* gepInst = llvm::dyn_cast<llvm::Instruction>(pointerOperand)) {
        const llvm::DebugLoc& loc = gepInst->getDebugLoc();
        if (loc) {
            gepCl = loc.getCol();
        }
    }
    if (gepCl < 0) {
        std::string gepStr = toString(pointerOperand);
        llvm::json::Object gepLocObj = parseSourceLocation(gepStr);
        if (auto cl = gepLocObj.getInteger("cl")) {
            gepCl = *cl;
        }
    }
    if (gepCl >= 0) {
        gepInfo["gep_cl"] = gepCl;
    } else {
        gepInfo["gep_cl"] = nullptr;
    }

    if (hasOffset) {
        gepInfo["offset"] = memberOffset;
    } else {
        gepInfo["offset"] = nullptr;
    }
    if (!structName.empty()) {
        gepInfo["baseobj_type"] = structName;
    } else if (basePointerValue && basePointerValue->getType()) {
        gepInfo["baseobj_type"] = toString(basePointerValue->getType());
    }

    result["gep_info"] = std::move(gepInfo);
    result["is_struct_lvalue"] = isStructLValue;
    result["is_member_access"] = isMember;
    result["is_lvar_param"] = pointerIsParam;
    result["is_lvar_baseobj_param"] = baseIsParam;
    if (!structName.empty()) {
        result["struct_name"] = structName;
    }

    return result;
}

const SVFGNode* getSVFGNodeFromActualINArg(SVFG* svfg,
                                           ICFG* icfg,
                                           SVFIR* pag,
                                           const std::string& location,
                                           int argIndex,
                                           const std::string& functionName) {
    // DEBUG
    // 这里的操作不现实 actualin节点太多了 别名也很难获取 很难真正实现
    if (!svfg || !icfg || !pag) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Invalid SVFG/ICFG/PAG pointer\n";
        return nullptr;
    }
    if (argIndex < 0) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Negative argument index\n";
        return nullptr;
    }

    const CallICFGNode* callNode = selectCallICFGNode(icfg, location, functionName);
    if (!callNode) {
        return nullptr;
    }

    // 打印所有callnode的所有svfg节点
    // Find and display all associated SVFG nodes
    std::vector<const SVFGNode*> associatedSVFGNodes;
    std::vector<const ActualINSVFGNode*> actualINSVFGNodes;
    for (auto& pair : *svfg) {
        SVFGNode* svfgNode = pair.second;
        if (svfgNode->getICFGNode() == callNode) {
            associatedSVFGNodes.push_back(svfgNode);
        }
    }
    SVF::SVFUtil::outs() << "[GraphReaderUtil] All SVFG nodes for call node " << callNode->getId() << ":\n";
    for (const SVFGNode* node : associatedSVFGNodes) {
        SVF::SVFUtil::outs() << "[GraphReaderUtil] SVFG node ID: " << node->getId() << "\n";
        // tostring
        SVF::SVFUtil::outs() << "[GraphReaderUtil] SVFG node toString: " << node->toString() << "\n";
        if (SVFUtil::isa<ActualINSVFGNode>(node)) {
            SVF::SVFUtil::outs() << "[GraphReaderUtil] ActualINSVFGNode ID: " << node->getId() << "\n";
            actualINSVFGNodes.push_back(SVFUtil::dyn_cast<ActualINSVFGNode>(node));
        }
    }
    // size
    SVF::SVFUtil::outs() << "[GraphReaderUtil] ActualINSVFGNodes size: " << actualINSVFGNodes.size() << "\n";

    if (!pag->hasCallSiteArgsMap(callNode)) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Call site '" << location
                             << "' has no recorded actual parameters\n";
        return nullptr;
    }
    const SVFIR::SVFVarList& args = pag->getCallSiteArgsList(callNode);
    if (argIndex >= static_cast<int>(args.size())) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Argument index " << argIndex
                             << " out of range (call has " << args.size() << " args)\n";
        return nullptr;
    }

    const SVFVar* argVar = args[argIndex];
    if (!argVar) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Null PAG node for argument " << argIndex
                             << "\n";
        return nullptr;
    }
    if (!argVar->isPointer()) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Argument " << argIndex
                             << " is not a pointer; cannot map to ActualINSVFGNode\n";
        return nullptr;
    }

    PointerAnalysis* pta = svfg->getPTA();
    if (!pta) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] SVFG has no pointer-analysis instance\n";
        return nullptr;
    }
    NodeBS argPts = pta->getPts(argVar->getId()).toNodeBS();
    if (argPts.empty()) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Argument " << argIndex
                             << " has empty points-to set; match may fail\n";
    }

    if (!svfg->hasActualINSVFGNodes(callNode)) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Call site '" << location
                             << "' has no ActualINSVFG nodes\n";
        return nullptr;
    }

    SVFG::ActualINSVFGNodeSet& actualIns = svfg->getActualINSVFGNodes(callNode);
    const ActualINSVFGNode* bestExact = nullptr;
    const ActualINSVFGNode* bestOverlap = nullptr;
    size_t bestExactSize = std::numeric_limits<size_t>::max();
    size_t bestOverlapSize = 0;

    for (auto it = actualIns.begin(), eit = actualIns.end(); it != eit; ++it) {
        const auto* actualIn = SVFUtil::dyn_cast<ActualINSVFGNode>(svfg->getSVFGNode(*it));
        if (!actualIn) {
            continue;
        }

        NodeBS regionPts = actualIn->getPointsTo();
        NodeBS intersection = regionPts;
        intersection &= argPts;

        size_t regionSize = regionPts.count();
        size_t overlapSize = intersection.count();
        if (overlapSize == 0) {
            continue;
        }

        if (overlapSize == regionSize && regionSize < bestExactSize) {
            bestExact = actualIn;
            bestExactSize = regionSize;
        }
        if (overlapSize > bestOverlapSize) {
            bestOverlap = actualIn;
            bestOverlapSize = overlapSize;
        }
    }

    const ActualINSVFGNode* chosen = bestExact ? bestExact : bestOverlap;
    if (!chosen) {
        SVF::SVFUtil::errs() << "[GraphReaderUtil] Unable to match ActualINSVFGNode for call '"
                             << location << "', arg_index=" << argIndex << "\n";
        return nullptr;
    }

    SVF::SVFUtil::outs() << "[GraphReaderUtil] Matched ActualINSVFGNode ID="
                         << chosen->getId() << " for call '" << location
                         << "', arg_index=" << argIndex << "\n";
    return chosen;
}

bool fetchFunctionStartLocation(SVFIR* pag, const std::string& funcName, std::string& startLocation) {
    const FunObjVar* funObj = pag->getFunObjVar(funcName);
    if (!funObj) {
        sendJsonError("Cannot find function '" + funcName + "'");
        return false;
    }

    const llvm::Value* funVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(funObj);
    const llvm::Function* llvmFun = SVF::SVFUtil::dyn_cast<llvm::Function>(funVal);
    if (!llvmFun) {
        sendJsonError("Cannot get LLVM function for '" + funcName + "'");
        return false;
    }

    llvm::json::Object funcInfo = getFunctionInfoJson(llvmFun);
    auto filenameOpt = funcInfo["filename"].getAsString();
    std::string filename = filenameOpt ? filenameOpt->str() : "";
    if (filename.empty()) {
        sendJsonError("Cannot get source location for function '" + funcName + "'");
        return false;
    }

    int64_t startLine = funcInfo["start_line"].getAsInteger().value_or(0);
    startLocation = filename + ":" + std::to_string(startLine);
    return true;
}

// recursively traces to ultimate source
// 这个函数最初是用来寻找一个左值转移 看这个左值最终被转移给了谁
// 要么是一个局部变量 store给了这个变量
// 要么是一个实参 被load到了实参当中 
// 很可能这个函数也有一些问题...
void tracePAGStore(SVFG* svfg, SVFIR* pag, const SVFVar* pagNode) {
    if (!svfg || !pag || !pagNode) {
        sendJsonError("Invalid input parameters for tracePAGStore");
        return;
    }
    
    // 构建 JSON 结果
    llvm::json::Object result;
    result["success"] = true;
    
    // Base PAG Node 信息（输入节点）
    llvm::json::Object basePAGInfo;
    basePAGInfo["id"] = static_cast<int64_t>(pagNode->getId());
    basePAGInfo["type"] = SVFUtil::isa<ValVar>(pagNode) ? "ValVar" : "ObjVar";
    basePAGInfo["desc"] = pagNode->toString();
    basePAGInfo["location"] = pagNode->getSourceLoc();
    result["base_pag_node"] = std::move(basePAGInfo);
    
    // 递归追踪 Definition SVFG Node
    const SVFVar* currentPAG = pagNode;
    int traceDepth = 0;
    const int MAX_TRACE_DEPTH = 10; // 防止无限循环
    
    while (traceDepth < MAX_TRACE_DEPTH && svfg->hasDefSVFGNode(currentPAG)) {
        const SVFGNode* defSVFGNode = svfg->getDefSVFGNode(currentPAG);
        std::string nodeKind = getSVFGNodeKindString(defSVFGNode, /*detailed=*/true);
        
        std::string defLocation = "";
        if (const ICFGNode* icfgNode = defSVFGNode->getICFGNode()) {
            defLocation = icfgNode->getSourceLoc();
        }
        
        // 保存第一层的定义信息（直接定义）
        if (traceDepth == 0) {
            llvm::json::Object directDefNode;
            directDefNode["svfg_id"] = static_cast<int64_t>(defSVFGNode->getId());
            directDefNode["kind"] = nodeKind;
            directDefNode["desc"] = defSVFGNode->toString();
            if (!defLocation.empty()) {
                directDefNode["location"] = defLocation;
            }
            result["direct_def_node"] = std::move(directDefNode);
        }
        
        // 如果是 LoadVFGNode，继续追踪其来源
        if (const LoadVFGNode* loadNode = SVFUtil::dyn_cast<LoadVFGNode>(defSVFGNode)) {
            // 获取 Load 语句
            const SVFStmt* stmt = loadNode->getPAGEdge();
            if (const LoadStmt* loadStmt = SVFUtil::dyn_cast<LoadStmt>(stmt)) {
                const SVFVar* loadFromPAG = loadStmt->getRHSVar(); // 从哪里 load
                
                // 先检查地址本身是否是变量定义 (AddrVFGNode)
                // 这对于本地变量非常重要，例如 *m++ 中的 m
                if (svfg->hasDefSVFGNode(loadFromPAG)) {
                    const SVFGNode* addrDefNode = svfg->getDefSVFGNode(loadFromPAG);
                    
                    // 如果地址是 AddrVFGNode (alloca)，需要区分两种情况
                    if (SVFUtil::isa<AddrVFGNode>(addrDefNode)) {
                        std::string addrNodeKind = getSVFGNodeKindString(addrDefNode, /*detailed=*/true);
                        std::string addrDefLocation = "";
                        if (const ICFGNode* icfgNode = addrDefNode->getICFGNode()) {
                            addrDefLocation = icfgNode->getSourceLoc();
                        }
                        
                        // 检查这个 alloca 是否用于存储函数参数
                        bool isParamShadow = false;
                        const SVFVar* paramValue = nullptr;
                        
                        for (auto it = loadFromPAG->getInEdges().begin(); 
                             it != loadFromPAG->getInEdges().end(); ++it) {
                            const SVFStmt* inStmt = *it;
                            if (const StoreStmt* store = SVFUtil::dyn_cast<StoreStmt>(inStmt)) {
                                const SVFVar* storedValue = store->getRHSVar();
                                
                                // 检查存储的值的定义是否是函数参数
                                if (svfg->hasDefSVFGNode(storedValue)) {
                                    const SVFGNode* storedDefNode = svfg->getDefSVFGNode(storedValue);
                                    if (SVFUtil::isa<FormalParmVFGNode>(storedDefNode)) {
                                        isParamShadow = true;
                                        paramValue = storedValue;
                                        break;
                                    }
                                }
                            }
                        }
                        
                        if (isParamShadow && paramValue) {
                            // 这是参数的影子副本，继续追踪参数
                            currentPAG = paramValue;
                            traceDepth++;
                            continue;  // 继续循环追踪
                        } else {
                            // 这是真正的局部变量定义
                            
                            // 保存最终来源信息
                            llvm::json::Object finalDefNode;
                            finalDefNode["svfg_id"] = static_cast<int64_t>(addrDefNode->getId());
                            finalDefNode["kind"] = addrNodeKind;
                            finalDefNode["desc"] = addrDefNode->toString();
                            if (!addrDefLocation.empty()) {
                                finalDefNode["location"] = addrDefLocation;
                            }
                            finalDefNode["pag_id"] = static_cast<int64_t>(loadFromPAG->getId());
                            finalDefNode["pag_desc"] = loadFromPAG->toString();
                            result["final_def_node"] = std::move(finalDefNode);
                            break;  // 找到真正的变量定义，停止追踪
                        }
                    }
                }
                
                // 如果不是 AddrVFGNode，继续查找对这个地址的 Store 操作
                bool foundStore = false;
                for (auto it = loadFromPAG->getInEdges().begin(); it != loadFromPAG->getInEdges().end(); ++it) {
                    const SVFStmt* inStmt = *it;
                    if (const StoreStmt* store = SVFUtil::dyn_cast<StoreStmt>(inStmt)) {
                        const SVFVar* storedValue = store->getRHSVar();
                        currentPAG = storedValue;
                        foundStore = true;
                        break;
                    }
                }
                
                if (!foundStore) {
                    // 如果没有找到 Store，检查 Load 地址本身的定义
                    // 这个地址可能是 GEP、Addr 等操作的结果
                    if (svfg->hasDefSVFGNode(loadFromPAG)) {
                        const SVFGNode* addrDefNode = svfg->getDefSVFGNode(loadFromPAG);
                        std::string addrNodeKind = getSVFGNodeKindString(addrDefNode, /*detailed=*/true);
                        
                        // 如果地址是 AddrVFGNode (alloca)，这就是变量定义的位置
                        if (SVFUtil::isa<AddrVFGNode>(addrDefNode)) {
                            
                            // 保存最终来源信息
                            std::string addrDefLocation = "";
                            if (const ICFGNode* icfgNode = addrDefNode->getICFGNode()) {
                                addrDefLocation = icfgNode->getSourceLoc();
                            }
                            
                            llvm::json::Object finalDefNode;
                            finalDefNode["svfg_id"] = static_cast<int64_t>(addrDefNode->getId());
                            finalDefNode["kind"] = addrNodeKind;
                            finalDefNode["desc"] = addrDefNode->toString();
                            if (!addrDefLocation.empty()) {
                                finalDefNode["location"] = addrDefLocation;
                            }
                            finalDefNode["pag_id"] = static_cast<int64_t>(loadFromPAG->getId());
                            finalDefNode["pag_desc"] = loadFromPAG->toString();
                            result["final_def_node"] = std::move(finalDefNode);
                            break;
                        }
                        // 如果地址是 GepVFGNode，停止追踪（结构体成员访问）
                        else if (SVFUtil::isa<GepVFGNode>(addrDefNode)) {
                            break;
                        }
                    }
                    break;
                }
            } else {
                break;
            }
        } 
        // 如果是 FormalParmVFGNode 或其他终止节点，停止追踪
        else if (SVFUtil::isa<FormalParmVFGNode>(defSVFGNode) || 
                 SVFUtil::isa<AddrVFGNode>(defSVFGNode) ||
                 SVFUtil::isa<ActualRetVFGNode>(defSVFGNode)) {
            // 保存最终来源信息
            llvm::json::Object finalDefNode;
            finalDefNode["svfg_id"] = static_cast<int64_t>(defSVFGNode->getId());
            finalDefNode["kind"] = nodeKind;
            finalDefNode["desc"] = defSVFGNode->toString();
            if (!defLocation.empty()) {
                finalDefNode["location"] = defLocation;
            }
            result["final_def_node"] = std::move(finalDefNode);
            break;
        }
        else {
            // 其他类型的节点，停止追踪
            break;
        }
        
        traceDepth++;
    }
    
    result["trace_depth"] = traceDepth;
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
}

void showCodeLineDebugInfo(SVFG* svfg, ICFG* icfg, const std::string& location) {
    // DEBUG
    // 重要功能 可以一直保留
    SVF::SVFUtil::outs() << "\n========================================\n";
    SVF::SVFUtil::outs() << "Debug Info for Location: " << location << "\n";
    SVF::SVFUtil::outs() << "========================================\n\n";

    // Find all ICFG nodes at this location
    std::vector<const ICFGNode*> allICFGNodes = findAllICFGNodesByLocation(icfg, location);
    
    if (allICFGNodes.empty()) {
        SVF::SVFUtil::errs() << "Error: No ICFG nodes found at location: " << location << "\n";
        sendJsonError("No ICFG nodes found at location: " + location);
        return;
    }

    SVF::SVFUtil::outs() << "Found " << allICFGNodes.size() << " ICFG node(s) at this location\n\n";

    // Process each ICFG node
    for (size_t i = 0; i < allICFGNodes.size(); i++) {
        const ICFGNode* icfgNode = allICFGNodes[i];
        
        SVF::SVFUtil::outs() << "----------------------------------------\n";
        SVF::SVFUtil::outs() << "ICFG Node #" << (i + 1) << ":\n";
        SVF::SVFUtil::outs() << "----------------------------------------\n";
        SVF::SVFUtil::outs() << "  ID: " << icfgNode->getId() << "\n";
        
        // Determine ICFG node type
        std::string nodeType = "Unknown";
        if (SVFUtil::isa<IntraICFGNode>(icfgNode)) {
            nodeType = "IntraICFGNode";
        } else if (SVFUtil::isa<CallICFGNode>(icfgNode)) {
            nodeType = "CallICFGNode";
        } else if (SVFUtil::isa<RetICFGNode>(icfgNode)) {
            nodeType = "RetICFGNode";
        } else if (SVFUtil::isa<FunEntryICFGNode>(icfgNode)) {
            nodeType = "FunEntryICFGNode";
        } else if (SVFUtil::isa<FunExitICFGNode>(icfgNode)) {
            nodeType = "FunExitICFGNode";
        } else if (SVFUtil::isa<GlobalICFGNode>(icfgNode)) {
            nodeType = "GlobalICFGNode";
        }
        SVF::SVFUtil::outs() << "  Type: " << nodeType << "\n";
        
        // Show source location with column info
        llvm::json::Object locInfo = parseSourceLocation(icfgNode->getSourceLoc());
        if (!locInfo.empty()) {
            if (auto file = locInfo.getString("fl")) {
                SVF::SVFUtil::outs() << "  File: " << file->str() << "\n";
            }
            if (auto line = locInfo.getInteger("ln")) {
                SVF::SVFUtil::outs() << "  Line: " << *line << "\n";
            }
            if (auto col = locInfo.getInteger("cl")) {
                SVF::SVFUtil::outs() << "  Column: " << *col << "\n";
            }
        }
        
        // Show full source location string
        SVF::SVFUtil::outs() << "  Full SourceLoc: " << icfgNode->getSourceLoc() << "\n";
        
        // Show LLVM Instruction information
        LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
        const llvm::Instruction* inst = nullptr;
        std::string instTypeStr = "None";
        
        if (const IntraICFGNode* intraNode = SVFUtil::dyn_cast<IntraICFGNode>(icfgNode)) {
            const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(intraNode);
            inst = llvm::dyn_cast<llvm::Instruction>(llvmVal);
            if (inst) {
                instTypeStr = inst->getOpcodeName();
                // Check if it's a ReturnInst
                if (llvm::isa<llvm::ReturnInst>(inst)) {
                    const llvm::ReturnInst* retInst = llvm::cast<llvm::ReturnInst>(inst);
                    instTypeStr += " (ReturnInst)";
                    llvm::Value* retVal = retInst->getReturnValue();
                    if (retVal) {
                        std::string retTypeStr;
                        llvm::raw_string_ostream rso(retTypeStr);
                        retVal->getType()->print(rso);
                        instTypeStr += " returns " + rso.str();
                    } else {
                        instTypeStr += " returns void";
                    }
                }
            }
        } else if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(icfgNode)) {
            const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(callNode);
            inst = llvm::dyn_cast<llvm::Instruction>(llvmVal);
            if (inst) {
                instTypeStr = inst->getOpcodeName();
            }
        } else if (const RetICFGNode* retNode = SVFUtil::dyn_cast<RetICFGNode>(icfgNode)) {
            const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(retNode);
            inst = llvm::dyn_cast<llvm::Instruction>(llvmVal);
            if (inst) {
                instTypeStr = inst->getOpcodeName();
            }
        }
        
        SVF::SVFUtil::outs() << "  LLVM Instruction Type: " << instTypeStr << "\n";
        if (const IntraICFGNode* intraNode = SVFUtil::dyn_cast<IntraICFGNode>(icfgNode)) {
            SVF::SVFUtil::outs() << "  Is Return Instruction: " << (intraNode->isRetInst() ? "true" : "false") << "\n";
        }
        
        // Show number of SVF statements
        SVF::SVFUtil::outs() << "  Number of SVF Statements: " << icfgNode->getSVFStmts().size() << "\n";
        
        // Find and display all associated SVFG nodes
        std::vector<const SVFGNode*> associatedSVFGNodes;
        for (auto& pair : *svfg) {
            SVFGNode* svfgNode = pair.second;
            if (svfgNode->getICFGNode() == icfgNode) {
                associatedSVFGNodes.push_back(svfgNode);
            }
        }
        
        SVF::SVFUtil::outs() << "  Number of SVFG Nodes: " << associatedSVFGNodes.size() << "\n\n";
        
        if (associatedSVFGNodes.empty()) {
            SVF::SVFUtil::outs() << "  No associated SVFG nodes found.\n";
        } else {
            SVF::SVFUtil::outs() << "  Associated SVFG Nodes:\n";
            for (size_t j = 0; j < associatedSVFGNodes.size(); j++) {
                const SVFGNode* svfgNode = associatedSVFGNodes[j];
                SVF::SVFUtil::outs() << "  ---\n";
                SVF::SVFUtil::outs() << "  SVFG Node #" << (j + 1) << ":\n";
                SVF::SVFUtil::outs() << "    SVFG Node ID: " << svfgNode->getId() << "\n";
                
                // Determine SVFG node type
                std::string svfgNodeType = "Unknown";
                if (SVFUtil::isa<StmtVFGNode>(svfgNode)) {
                    svfgNodeType = "StmtVFGNode";
                    // Further classify StmtVFGNode
                    if (SVFUtil::isa<AddrVFGNode>(svfgNode)) {
                        svfgNodeType = "AddrVFGNode";
                    } else if (SVFUtil::isa<CopyVFGNode>(svfgNode)) {
                        svfgNodeType = "CopyVFGNode";
                    } else if (SVFUtil::isa<GepVFGNode>(svfgNode)) {
                        svfgNodeType = "GepVFGNode";
                    } else if (SVFUtil::isa<StoreVFGNode>(svfgNode)) {
                        svfgNodeType = "StoreVFGNode";
                    } else if (SVFUtil::isa<LoadVFGNode>(svfgNode)) {
                        svfgNodeType = "LoadVFGNode";
                    }
                } else if (SVFUtil::isa<PHIVFGNode>(svfgNode)) {
                    svfgNodeType = "PHIVFGNode";
                    if (SVFUtil::isa<IntraPHIVFGNode>(svfgNode)) {
                        svfgNodeType = "IntraPHIVFGNode";
                    } else if (SVFUtil::isa<InterPHIVFGNode>(svfgNode)) {
                        svfgNodeType = "InterPHIVFGNode";
                    }
                } else if (SVFUtil::isa<MRSVFGNode>(svfgNode)) {
                    svfgNodeType = "MRSVFGNode";
                    if (SVFUtil::isa<FormalINSVFGNode>(svfgNode)) {
                        svfgNodeType = "FormalINSVFGNode";
                    } else if (SVFUtil::isa<FormalOUTSVFGNode>(svfgNode)) {
                        svfgNodeType = "FormalOUTSVFGNode";
                    } else if (SVFUtil::isa<ActualINSVFGNode>(svfgNode)) {
                        svfgNodeType = "ActualINSVFGNode";
                    } else if (SVFUtil::isa<ActualOUTSVFGNode>(svfgNode)) {
                        svfgNodeType = "ActualOUTSVFGNode";
                    } else if (SVFUtil::isa<MSSAPHISVFGNode>(svfgNode)) {
                        svfgNodeType = "MSSAPHISVFGNode";
                        if (SVFUtil::isa<IntraMSSAPHISVFGNode>(svfgNode)) {
                            svfgNodeType = "IntraMSSAPHISVFGNode";
                        } else if (SVFUtil::isa<InterMSSAPHISVFGNode>(svfgNode)) {
                            svfgNodeType = "InterMSSAPHISVFGNode";
                        }
                    }
                } else if (SVFUtil::isa<FormalParmVFGNode>(svfgNode)) {
                    svfgNodeType = "FormalParmVFGNode";
                } else if (SVFUtil::isa<ActualParmVFGNode>(svfgNode)) {
                    svfgNodeType = "ActualParmVFGNode";
                } else if (SVFUtil::isa<FormalRetVFGNode>(svfgNode)) {
                    svfgNodeType = "FormalRetVFGNode";
                } else if (SVFUtil::isa<ActualRetVFGNode>(svfgNode)) {
                    svfgNodeType = "ActualRetVFGNode";
                }
                
                SVF::SVFUtil::outs() << "    SVFG Node Type: " << svfgNodeType << "\n";
                SVF::SVFUtil::outs() << "    Node String: " << svfgNode->toString() << "\n";
                
                // Show incoming and outgoing edges count
                int inEdgeCount = 0;
                int outEdgeCount = 0;
                for (auto it = svfgNode->InEdgeBegin(); it != svfgNode->InEdgeEnd(); ++it) {
                    inEdgeCount++;
                }
                for (auto it = svfgNode->OutEdgeBegin(); it != svfgNode->OutEdgeEnd(); ++it) {
                    outEdgeCount++;
                }
                SVF::SVFUtil::outs() << "    Incoming Edges: " << inEdgeCount << "\n";
                SVF::SVFUtil::outs() << "    Outgoing Edges: " << outEdgeCount << "\n";
            }
        }
        
        SVF::SVFUtil::outs() << "\n";
    }
    
    SVF::SVFUtil::outs() << "========================================\n";
    SVF::SVFUtil::outs() << "End of Debug Info\n";
    SVF::SVFUtil::outs() << "========================================\n\n";

    // Send JSON success response
    llvm::json::Object result;
    result["success"] = true;
    result["location"] = location;
    result["icfg_nodes_count"] = static_cast<int64_t>(allICFGNodes.size());
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
}

} // namespace GraphReaderUtil
} // namespace SVF
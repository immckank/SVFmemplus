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

namespace SVF {
namespace GraphReaderUtil {

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

std::string getSVFGNodeKindString(const SVFGNode* node) {
    if (!node) return "Null";
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
        default: return "Unknown";
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
    
    return args[argIndex];
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

const PAGNode* getPAGNodeFromLvarGEP(ICFG* icfg, SVFIR* pag, const std::string& location, int eqPosition) {
    if (!icfg || !pag) {
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

const PAGNode* getPAGNodeFromCallArg(ICFG* icfg, SVFIR* pag, const std::string& location, int argIndex) {
    if (!icfg || !pag) {
        return nullptr;
    }
    
    // Find all ICFG nodes at this location
    std::vector<const ICFGNode*> allICFGNodes = findAllICFGNodesByLocation(icfg, location);
    if (allICFGNodes.empty()) {
        return nullptr;
    }

    // Find the CallICFGNode
    const CallICFGNode* callNode = nullptr;
    for (const ICFGNode* node : allICFGNodes) {
        if (auto cn = SVFUtil::dyn_cast<CallICFGNode>(node)) {
            callNode = cn;
            break;
        }
    }

    if (!callNode) {
        return nullptr;
    }

    // Get the llvm::CallInst to determine argument order
    const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(callNode);
    const llvm::CallBase* callInst = SVFUtil::dyn_cast<llvm::CallBase>(llvmVal);
    
    if (!callInst) {
        return nullptr;
    }

    if (argIndex < 0 || argIndex >= static_cast<int>(callInst->arg_size())) {
        return nullptr;
    }

    // Get the target argument llvm::Value
    const llvm::Value* targetArgVal = callInst->getArgOperand(argIndex);
    
    // Find the corresponding PAGNode for this argument
    NodeID argNodeID = LLVMModuleSet::getLLVMModuleSet()->getValueNode(targetArgVal);
    if (argNodeID != UINT_MAX && pag->hasGNode(argNodeID)) {
        return pag->getGNode(argNodeID);
    }
    
    return nullptr;
}

// Helper function to determine SVFG node kind
static std::string getSVFGNodeKind(const SVFGNode* node) {
    if (SVFUtil::isa<StoreVFGNode>(node)) return "StoreVFGNode";
    if (SVFUtil::isa<LoadVFGNode>(node)) return "LoadVFGNode";
    if (SVFUtil::isa<CopyVFGNode>(node)) return "CopyVFGNode";
    if (SVFUtil::isa<GepVFGNode>(node)) return "GepVFGNode";
    if (SVFUtil::isa<AddrVFGNode>(node)) return "AddrVFGNode";
    if (SVFUtil::isa<PHIVFGNode>(node)) return "PHIVFGNode";
    if (SVFUtil::isa<FormalParmVFGNode>(node)) return "FormalParmVFGNode";
    if (SVFUtil::isa<ActualParmVFGNode>(node)) return "ActualParmVFGNode";
    if (SVFUtil::isa<FormalRetVFGNode>(node)) return "FormalRetVFGNode";
    if (SVFUtil::isa<ActualRetVFGNode>(node)) return "ActualRetVFGNode";
    if (SVFUtil::isa<MRSVFGNode>(node)) return "MRSVFGNode";
    return "VFGNode";
}

// Simplified function: recursively traces to ultimate source
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
        std::string nodeKind = getSVFGNodeKind(defSVFGNode);
        
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
                        std::string addrNodeKind = getSVFGNodeKind(addrDefNode);
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
                        std::string addrNodeKind = getSVFGNodeKind(addrDefNode);
                        
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

const SVFGNode* findMemoryDefNode(SVFG* svfg, const SVFVar* addressPAG) {
    if (!svfg || !addressPAG) {
        return nullptr;
    }

    // Strategy 1: Use SVFG backward traversal from load to find store via def-use edges
    // First, find the LoadVFGNode that uses this address
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
        // Traverse backward through SVFG edges to find the store definition
        for (auto it = loadNode->InEdgeBegin(); it != loadNode->InEdgeEnd(); ++it) {
            const SVFGEdge* edge = *it;
            const SVFGNode* srcNode = edge->getSrcNode();
            
            // Check if source is a StoreVFGNode
            if (const auto* storeNode = SVFUtil::dyn_cast<StoreVFGNode>(srcNode)) {
                return storeNode;
            }
            
            // Check if source is an MSSA node (indirect def)
            if (const auto* mssaNode = SVFUtil::dyn_cast<MRSVFGNode>(srcNode)) {
                // Continue searching through MSSA node's predecessors
                for (auto it2 = mssaNode->InEdgeBegin(); it2 != mssaNode->InEdgeEnd(); ++it2) {
                    const SVFGEdge* edge2 = *it2;
                    const SVFGNode* srcNode2 = edge2->getSrcNode();
                    if (const auto* storeNode = SVFUtil::dyn_cast<StoreVFGNode>(srcNode2)) {
                        return storeNode;
                    }
                }
            }
        }
    }
    
    // Strategy 2: If SVFG edge traversal fails, try matching by GEP structure
    
    std::vector<const SVFGNode*> candidateStores;
    
    // Extract GEP information from the address
    const GepValVar* addressGep = SVFUtil::dyn_cast<GepValVar>(addressPAG);
    if (!addressGep) {
        return nullptr;
    }
    
    // Search for stores with matching GEP structure
    for (auto it = svfg->begin(); it != svfg->end(); ++it) {
        const SVFGNode* node = it->second;
        
        if (const auto* storeNode = SVFUtil::dyn_cast<StoreVFGNode>(node)) {
            const SVFStmt* pagEdge = storeNode->getPAGEdge();
            const StoreStmt* storeStmt = SVFUtil::dyn_cast<StoreStmt>(pagEdge);
            if (storeStmt) {
                const SVFVar* storeLHS = storeStmt->getLHSVar();
                
                // Check if store target is also a GEP with same structure
                if (const auto* storeLHSGep = SVFUtil::dyn_cast<GepValVar>(storeLHS)) {
                    // Compare field index (offset)
                    if (storeLHSGep->getConstantFieldIdx() == addressGep->getConstantFieldIdx()) {
                        candidateStores.push_back(storeNode);
                    }
                }
            }
        }
    }
    
    if (candidateStores.empty()) {
        return nullptr;
    }
    
    // Return the first candidate
    return candidateStores[0];
}

void traceCallArgumentToPAGNode(SVFG* svfg, ICFG* icfg, SVFIR* pag, const std::string& location, int argIndex) {
    SVF::SVFUtil::outs() << "\n========================================\n";
    SVF::SVFUtil::outs() << "Tracing Call Argument to PAGNode\n";
    SVF::SVFUtil::outs() << "Location: " << location << "\n";
    SVF::SVFUtil::outs() << "Argument Index: " << argIndex << "\n";
    SVF::SVFUtil::outs() << "========================================\n\n";

    // Part 1: Get PAGNode from call argument
    const PAGNode* pagNode = getPAGNodeFromCallArg(icfg, pag, location, argIndex);
    
    if (!pagNode) {
        SVF::SVFUtil::errs() << "Error: Could not get PAGNode for argument " << argIndex 
                             << " at location: " << location << "\n";
        sendJsonError("Could not get PAGNode for argument");
        return;
    }

    // Part 2: Trace PAG store for this PAGNode
    tracePAGStore(svfg, pag, pagNode);
    
    SVF::SVFUtil::outs() << "\n========================================\n";
    SVF::SVFUtil::outs() << "End of Trace\n";
    SVF::SVFUtil::outs() << "========================================\n\n";
}

void showCodeLineDebugInfo(SVFG* svfg, ICFG* icfg, const std::string& location) {
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

const PAGNode* traceCallArgumentValueFlow(SVFG* svfg, ICFG* icfg, SVFIR* pag, const std::string& callLocation, int argIndex) {
    // Step 1: Find the CallICFGNode at the call location
    std::vector<const ICFGNode*> allICFGNodes = findAllICFGNodesByLocation(icfg, callLocation);
    if (allICFGNodes.empty()) {
        SVF::SVFUtil::errs() << "Error: No ICFG nodes found at location: " << callLocation << "\n";
        return nullptr;
    }

    const CallICFGNode* callNode = nullptr;
    for (const ICFGNode* node : allICFGNodes) {
        if (auto cn = SVFUtil::dyn_cast<CallICFGNode>(node)) {
            callNode = cn;
            break;
        }
    }

    if (!callNode) {
        SVF::SVFUtil::errs() << "Error: No CallICFGNode found at location: " << callLocation << "\n";
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

    if (actualParmNodes.empty()) {
        SVF::SVFUtil::errs() << "Error: No ActualParmVFGNode found for this call\n";
        return nullptr;
    }

    // Get the llvm::CallInst to determine argument order
    const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(callNode);
    const llvm::CallBase* callInst = SVFUtil::dyn_cast<llvm::CallBase>(llvmVal);
    
    if (!callInst) {
        SVF::SVFUtil::errs() << "Error: Could not get CallBase from CallICFGNode\n";
        return nullptr;
    }

    if (argIndex < 0 || argIndex >= static_cast<int>(callInst->arg_size())) {
        SVF::SVFUtil::errs() << "Error: Invalid argument index " << argIndex << "\n";
        return nullptr;
    }

    // Get the target argument llvm::Value
    const llvm::Value* targetArgVal = callInst->getArgOperand(argIndex);
    
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
        SVF::SVFUtil::errs() << "Error: Could not find ActualParmVFGNode for argument " << argIndex << "\n";
        return nullptr;
    }
    
    // Step 3: Get the PAGNode from the ActualParmVFGNode
    const SVFVar* paramPAG = targetParam->getParam();

    // Step 4: Check if this is a load instruction
    const llvm::Value* paramLLVMVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(paramPAG);
    if (!paramLLVMVal) {
        return paramPAG;
    }

    const llvm::LoadInst* loadInst = SVFUtil::dyn_cast<llvm::LoadInst>(paramLLVMVal);
    if (!loadInst) {
        return paramPAG;
    }
    
    // Step 5: Get the address being loaded from (typically a GEP result)
    const llvm::Value* ptrOperand = loadInst->getPointerOperand();
    NodeID ptrNodeID = LLVMModuleSet::getLLVMModuleSet()->getValueNode(ptrOperand);
    
    if (ptrNodeID == UINT_MAX || !pag->hasGNode(ptrNodeID)) {
        SVF::SVFUtil::errs() << "Error: Could not find PAGNode for load pointer operand\n";
        return nullptr;
    }

    const SVFVar* addressPAG = pag->getGNode(ptrNodeID);

    // Step 6: Find the store definition for this address
    const SVFGNode* storeNode = findMemoryDefNode(svfg, addressPAG);
    
    if (!storeNode) {
        SVF::SVFUtil::errs() << "Warning: Could not find store definition for this address\n";
        return nullptr;
    }

    // Step 7: Extract the stored value from the store node
    const PAGNode* storedValuePAG = nullptr;
    
    if (const auto* storeVFGNode = SVFUtil::dyn_cast<StoreVFGNode>(storeNode)) {
        const SVFStmt* pagEdge = storeVFGNode->getPAGEdge();
        const StoreStmt* storeStmt = SVFUtil::dyn_cast<StoreStmt>(pagEdge);
        if (storeStmt) {
            storedValuePAG = storeStmt->getRHSVar();
        }
    }

    return storedValuePAG;
}

} // namespace GraphReaderUtil
} // namespace SVF
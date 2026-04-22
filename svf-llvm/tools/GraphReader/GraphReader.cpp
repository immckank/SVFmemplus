#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVFIR/SVFValue.h"
#include "WPA/Andersen.h"
#include "MSSA/MemSSA.h"
#include "Graphs/SVFG.h"
#include "Graphs/CallGraph.h"
#include "Graphs/ICFG.h"
#include "SABER/SaberCondAllocator.h"
#include "Util/Options.h"
#include "Util/CDGBuilder.h"
// #include "GraphReader/SVFGChecker.h"
#include "GraphReaderSVFGBuilder.h"
#include "GraphReaderUtil.h"
#include "PathQuery.h"
#include "FunctionQuery.h"
#include <llvm/IR/DebugInfo.h>
#include <llvm/Support/JSON.h>
#include <map>
#include <memory>

using namespace llvm;
using namespace SVF;
using namespace SVFUtil;

/*!
 * GraphReader: A tool to read and analyze SVF graphs.
 * It uses SVF's command-line option system for analysis selection.
 */
int main(int argc, char ** argv) {
    //现在graph-reader只接受一次bc文件输入
    //随后一直接受各种json格式的分析选项
    // 首次启动分析 graph-reader <input-bitcode>
    std::vector<std::string> moduleNameVec = 
        OptionBase::parseOptions(argc, argv, "GraphReader", "[options] <input-bitcode...>");

    LLVMModuleSet::buildSVFModule(moduleNameVec);
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();

    AndersenWaveDiff* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    ICFG* icfg = ander->getICFG();

    // Use GraphReaderSVFGBuilder - a specialized builder for GraphReader
    // Keep builder on heap to prevent SVFG from being destroyed (SVFG is owned by builder)
    GraphReaderSVFGBuilder* memSSA = new GraphReaderSVFGBuilder(true, false);
    
    // Optional: Enable Saber optimizations if needed
    memSSA->setEnableSaberOptimizations(true);
    
    SVFG* svfg = memSSA->buildFullSVFG(ander);

    // Pre-compute all functions that call free (silently, no output)
    SVF::GraphReaderUtil::findAllFreeCallers(pag, true);
    
    auto saberCondAllocator = std::make_unique<SaberCondAllocator>();
    saberCondAllocator->allocate();
    SVF::GraphReaderUtil::setSaberCondAllocator(saberCondAllocator.get());

    FunctionQuery fq(icfg, pag, svfg);
    PathQuery pq(svfg, icfg);
    {
        llvm::json::Object ready;
        ready["ready"] = true;
        ready["message"] = "graphreader-initialized";
        llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(ready))) << "\n";
        llvm::outs().flush();
    }
    
    // 作为长驻进程，按行读取 JSON 请求，仅在收到 exit 或 EOF 时退出
    while (true) {
        std::string jsonInput;
        if (!std::getline(std::cin, jsonInput)) {
            break; // EOF
        }
        if (jsonInput.empty()) {
            continue;
        }

        std::string errMsg;
        std::vector<llvm::json::Object> cmds;
        if (!SVF::GraphReaderUtil::parseCommandsLine(jsonInput, cmds, errMsg)) {
            SVF::GraphReaderUtil::sendJsonError("json parse error: " + errMsg);
            llvm::outs().flush();
            llvm::outs().flush();
            continue;
        }

        bool shouldExit = false;
        for (auto &cmd : cmds) {
            std::string cname;
            if (auto s = cmd.getString("command")) {
                cname = s->str();
            } else {
                SVF::GraphReaderUtil::sendJsonError("missing 'command'");
                continue;
            }

            auto sendStringError = [](const std::string& message) {
                llvm::json::Object result;
                result["error"] = message;
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
            };

            auto readStructuredLocation = [](const llvm::json::Object& cmdObj,
                                             const char* legacyLocationKey,
                                             SVF::GraphReaderUtil::SourceLocation& outLocation,
                                             int64_t* outColumn,
                                             std::string& errorMessage) -> bool {
                if (auto fl = cmdObj.getString("fl")) {
                    auto ln = cmdObj.getInteger("ln");
                    if (!ln) {
                        errorMessage = "missing 'ln'";
                        return false;
                    }
                    outLocation.fl = fl->str();
                    outLocation.ln = *ln;
                    outLocation.cl = -1;
                    if (outColumn) {
                        if (auto cl = cmdObj.getInteger("cl")) {
                            *outColumn = *cl;
                            outLocation.cl = *cl;
                        } else {
                            *outColumn = -1;
                        }
                    }
                    return true;
                }
                if (auto loc = cmdObj.getString(legacyLocationKey)) {
                    outLocation = SVF::GraphReaderUtil::parseSourceLocationStruct(loc->str());
                    if (!outLocation.isValid()) {
                        size_t colonPos = loc->str().find(':');
                        if (colonPos != std::string::npos) {
                            outLocation.fl = loc->str().substr(0, colonPos);
                            try {
                                std::string tail = loc->str().substr(colonPos + 1);
                                size_t secondColonPos = tail.find(':');
                                if (secondColonPos != std::string::npos) {
                                    outLocation.ln = std::stoll(tail.substr(0, secondColonPos));
                                    outLocation.cl = std::stoll(tail.substr(secondColonPos + 1));
                                } else {
                                    outLocation.ln = std::stoll(tail);
                                }
                            } catch (const std::exception&) {
                                outLocation.ln = 0;
                            }
                        }
                    }
                    if (outColumn) {
                        if (auto cl = cmdObj.getInteger("cl")) {
                            *outColumn = *cl;
                            outLocation.cl = *cl;
                        } else {
                            *outColumn = -1;
                        }
                    }
                    return outLocation.isValid();
                }
                errorMessage = std::string("missing '") + legacyLocationKey + "' or ('fl' and 'ln')";
                return false;
            };

            auto readPrefixedStructuredLocation = [&](const llvm::json::Object& cmdObj,
                                                      const char* prefix,
                                                      const char* legacyLocationKey,
                                                      SVF::GraphReaderUtil::SourceLocation& outLocation,
                                                      std::string& errorMessage) -> bool {
                const std::string prefixStr(prefix);
                const std::string flKey = prefixStr + "fl";
                const std::string lnKey = prefixStr + "ln";
                const std::string clKey = prefixStr + "cl";

                if (auto fl = cmdObj.getString(flKey)) {
                    auto ln = cmdObj.getInteger(lnKey);
                    if (!ln) {
                        errorMessage = "missing '" + lnKey + "'";
                        return false;
                    }
                    outLocation.fl = fl->str();
                    outLocation.ln = *ln;
                    outLocation.cl = -1;
                    if (auto cl = cmdObj.getInteger(clKey)) {
                        outLocation.cl = *cl;
                    }
                    return true;
                }

                if (legacyLocationKey != nullptr) {
                    return readStructuredLocation(cmdObj, legacyLocationKey, outLocation, nullptr, errorMessage);
                }

                errorMessage = "missing '" + flKey + "' and '" + lnKey + "'";
                return false;
            };

            auto appendStructuredLocationFields = [](llvm::json::Object& out,
                                                     const llvm::json::Object& locObj) {
                if (auto fl = locObj.getString("fl")) {
                    out["fl"] = fl->str();
                    out["filename"] = fl->str();
                } else if (auto file = locObj.getString("file")) {
                    out["fl"] = file->str();
                    out["filename"] = file->str();
                }
                if (auto ln = locObj.getInteger("ln")) {
                    out["ln"] = *ln;
                    out["line"] = *ln;
                }
                if (auto cl = locObj.getInteger("cl")) {
                    out["cl"] = *cl;
                    out["column"] = *cl;
                }
            };

            // function TODOs: 
            // 1. 在某一个赋值语句处 构建一个针对左值的GEP操作栈
            // 2. 在所有find value path index系列中 将GEP栈作为一个可选的传入变量 
            // 我们能知道目前追踪的是什么左值 真正持有内存的是哪个成员变量 
            // 3. 在所有find value path index系列中 将所有icfg不可达到返回位置也加入
            // 原因是 icfg上的遍历在面对循环时可能出现将可到达的位置判定为不可达 从而导致我们漏掉一些路径
            if (cname == "find-function-body-by-name") {
                if (auto n = cmd.getString("name")) {
                    fq.findFunctionBodyByName(n->str());
                } else {
                    SVF::GraphReaderUtil::sendJsonError("missing 'name'");
                }
            } else if (cname == "find-function-body-by-location") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                if (readStructuredLocation(cmd, "location", location, nullptr, locationError)) {
                    fq.findFunctionBodyByLocation(location);
                } else {
                    SVF::GraphReaderUtil::sendJsonError(locationError);
                }
            } else if (cname == "find-all-function-call-sites") {
                if (auto n = cmd.getString("name")) {
                    fq.findCallSites(n->str());
                } else {
                    SVF::GraphReaderUtil::sendJsonError("missing 'name'");
                }
            } else if (cname == "find-all-function-callees") {
                if (auto n = cmd.getString("name")) {
                    fq.findAllCalleesByName(n->str());
                } else {
                    SVF::GraphReaderUtil::sendJsonError("missing 'name'");
                }
            } else if (cname == "find-cond-path") {
                SVF::GraphReaderUtil::SourceLocation startLocation;
                SVF::GraphReaderUtil::SourceLocation targetLocation;
                std::string startError;
                std::string targetError;
                if (!readPrefixedStructuredLocation(cmd, "start_", "start", startLocation, startError) ||
                    !readPrefixedStructuredLocation(cmd, "target_", "end", targetLocation, targetError)) {
                    SVF::GraphReaderUtil::sendJsonError("missing start_fl/start_ln or target_fl/target_ln");
                } else {
                    pq.getConditionPath(startLocation, targetLocation);
                }
            } else if (cname == "find-cond-path-inside") {
                SVF::GraphReaderUtil::SourceLocation startLocation;
                SVF::GraphReaderUtil::SourceLocation targetLocation;
                std::string startError;
                std::string targetError;
                if (!readPrefixedStructuredLocation(cmd, "start_", "start", startLocation, startError) ||
                    !readPrefixedStructuredLocation(cmd, "target_", "end", targetLocation, targetError)) {
                    SVF::GraphReaderUtil::sendJsonError("missing start_fl/start_ln or target_fl/target_ln");
                } else {
                    pq.getConditionInsidePath(startLocation, targetLocation);
                }
            } else if (cname == "find-arg-value-path-inside") {
                auto functionName = cmd.getString("function_name");
                auto argIndexStr = cmd.getString("arg_index");
                if (!functionName || !argIndexStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'function_name' or 'arg_index'");
                    continue;
                }
                int argIndex = -1;
                std::string functionNameStr = functionName ? functionName->str() : "";
                std::string startLocation;
                if (!SVF::GraphReaderUtil::fetchFunctionStartLocation(pag, functionNameStr, startLocation)) {
                    //send json error
                    SVF::GraphReaderUtil::sendJsonError("Cannot find start location for function '" + functionNameStr + "'");
                    continue;
                }
                try {
                    argIndex = std::stoi(argIndexStr->str());
                } catch (...) {
                    SVF::GraphReaderUtil::sendJsonError("invalid 'arg_index' value: " + argIndexStr->str());
                    continue;
                }
                std::vector<const SVFGNode*> startSVFGNodes;
                const PAGNode* startPAGNode = SVF::GraphReaderUtil::getPAGNodeFromArg(pag, functionNameStr, argIndex);
                if (startPAGNode) {
                    const SVFGNode* startSVFGNode = svfg->getDefSVFGNode(startPAGNode);
                    if (startSVFGNode) {
                        startSVFGNodes.push_back(startSVFGNode);
                    }
                }
                pq.getValueSensitiveReturnInsidePath(SVF::GraphReaderUtil::parseSourceLocationStruct(startLocation), startSVFGNodes);
            } else if (cname == "find-lvalue-path-inside") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                auto eqPositionStr = cmd.getString("eq_position");
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError) || !eqPositionStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing location/fl+ln or 'eq_position'");
                } else {
                    int eqPosition = -1;
                    try {
                        eqPosition = std::stoi(eqPositionStr->str());
                    } catch (...) {
                        SVF::GraphReaderUtil::sendJsonError("invalid 'eq_position' value: " + eqPositionStr->str());
                        continue;
                    }
                    std::vector<const SVFGNode*> startSVFGNodes;
                    if (eqPosition != -1) {
                        const PAGNode* startPAGNode = SVF::GraphReaderUtil::getPAGNodeFromLvar(icfg, pag, location, eqPosition);
                        if (!startPAGNode) {
                            SVF::GraphReaderUtil::sendJsonError("Cannot find PAGNode for Lvar at location '" + SVF::GraphReaderUtil::toString(location) + "' with eq_position " + std::to_string(eqPosition));
                            continue;
                        }
                        const SVFGNode* startSVFGNode = svfg->getDefSVFGNode(startPAGNode);
                        if (!startSVFGNode) {
                            SVF::GraphReaderUtil::sendJsonError("Cannot find SVFGNode for PAGNode " + std::to_string(startPAGNode->getId()));
                            continue;
                        }
                        startSVFGNodes.push_back(startSVFGNode);
                    }
                    pq.getValueSensitiveReturnInsidePath(location, startSVFGNodes);
                }
            } else if (cname == "find-lvalue-detail-path-inside") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                auto eqPositionStr = cmd.getString("eq_position");
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError) || !eqPositionStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing location/fl+ln or 'eq_position'");
                } else {
                    int eqPosition = -1;
                    try {
                        eqPosition = std::stoi(eqPositionStr->str());
                    } catch (...) {
                        SVF::GraphReaderUtil::sendJsonError("invalid 'eq_position' value: " + eqPositionStr->str());
                        continue;
                    }
                    std::vector<const SVFGNode*> startSVFGNodes;
                    if (eqPosition != -1) {
                        const PAGNode* startPAGNode = SVF::GraphReaderUtil::getPAGNodeFromLvar(icfg, pag, location, eqPosition);
                        if (!startPAGNode) {
                            SVF::GraphReaderUtil::sendJsonError("Cannot find PAGNode for Lvar at location '" + SVF::GraphReaderUtil::toString(location) + "' with eq_position " + std::to_string(eqPosition));
                            continue;
                        }
                        const SVFGNode* startSVFGNode = svfg->getDefSVFGNode(startPAGNode);
                        if (!startSVFGNode) {
                            SVF::GraphReaderUtil::sendJsonError("Cannot find SVFGNode for PAGNode " + std::to_string(startPAGNode->getId()));
                            continue;
                        }
                        startSVFGNodes.push_back(startSVFGNode);
                    }
                    pq.getValueSensitiveReturnInsidePathDetailed(location, startSVFGNodes);
                }
            } else if (cname == "find-lvalue-detail-path-inside-store") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                auto eqPositionStr = cmd.getString("eq_position");
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError) || !eqPositionStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing location/fl+ln or 'eq_position'");
                    continue;
                }
                int eqPosition = -1;
                try {
                    eqPosition = std::stoi(eqPositionStr->str());
                } catch (...) {
                    SVF::GraphReaderUtil::sendJsonError("invalid 'eq_position' value: " + eqPositionStr->str());
                    continue;
                }
                // 直接寻找cl的store路径
                std::vector<const ICFGNode*> nodes = SVF::GraphReaderUtil::findAllICFGNodesByLocation(icfg, location);
                const ICFGNode* matchedNode = nullptr;
                for (const ICFGNode* node : nodes) {
                    if (!SVFUtil::isa<IntraICFGNode>(node)) {
                        continue;
                    }
                    llvm::json::Object locInfo = SVF::GraphReaderUtil::parseSourceLocation(node->getSourceLoc());
                    if (auto col = locInfo.getInteger("cl")) {
                        if (*col == eqPosition) {
                            matchedNode = node;
                            break;
                        }
                    }
                }
                std::vector<const SVFGNode*> storeNodes;
                for (auto& pair : *svfg) {
                    SVFGNode* svfgNode = pair.second;
                    if (svfgNode->getICFGNode() == matchedNode) {
                        if (auto storeNode = SVFUtil::dyn_cast<StoreVFGNode>(svfgNode)) {
                            storeNodes.push_back(storeNode);
                        }
                    }
                }
                std::vector<const SVFGNode*> startSVFGNodes;
                for (const SVFGNode* storeNode : storeNodes) {
                    startSVFGNodes.push_back(storeNode);
                }
                pq.getValueSensitiveReturnInsidePathDetailed(location, startSVFGNodes);
            } else if (cname == "find-lvalue-key_svfgnode") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                auto eqPositionStr = cmd.getString("eq_position");
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError) || !eqPositionStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing location/fl+ln or 'eq_position'");
                    continue;
                }
                int eqPosition = -1;
                try {
                    eqPosition = std::stoi(eqPositionStr->str());
                } catch (...) {
                    SVF::GraphReaderUtil::sendJsonError("invalid 'eq_position' value: " + eqPositionStr->str());
                    continue;
                }
                // Parse optional offsets array
                std::vector<std::string> offsets;
                if (auto offsetsArray = cmd.getArray("offsets")) {
                    for (const auto& offsetVal : *offsetsArray) {
                        if (auto offsetStr = offsetVal.getAsString()) {
                            offsets.push_back(offsetStr->str());
                        } else {
                            SVF::GraphReaderUtil::sendJsonError("invalid offset value in 'offsets' array (must be strings)");
                            continue;
                        }
                    }
                }
                pq.findLvalueKeySVFGNodes(location, eqPosition, offsets);
            } else if (cname == "find-formal_arg-key_svfgnode") {
                auto functionName = cmd.getString("function_name");
                auto argIndexStr = cmd.getString("arg_index");
                if (!functionName || !argIndexStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'function_name' or 'arg_index'");
                    continue;
                }
                int argIndex = -1;
                try {
                    argIndex = std::stoi(argIndexStr->str());
                } catch (...) {
                    SVF::GraphReaderUtil::sendJsonError("invalid 'arg_index' value: " + argIndexStr->str());
                    continue;
                }
                // Parse optional offsets array
                std::vector<std::string> offsets;
                if (auto offsetsArray = cmd.getArray("offsets")) {
                    for (const auto& offsetVal : *offsetsArray) {
                        if (auto offsetStr = offsetVal.getAsString()) {
                            offsets.push_back(offsetStr->str());
                        } else {
                            SVF::GraphReaderUtil::sendJsonError("invalid offset value in 'offsets' array (must be strings)");
                            continue;
                        }
                    }
                }
                pq.findFormalArgKeySVFGNodes(functionName->str(), argIndex, offsets);
            } else if (cname == "find-actual_arg-key_svfgnode") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                auto calleeFuncName = cmd.getString("callee_function_name");
                auto argIndexStr = cmd.getString("arg_index");
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError) || !calleeFuncName || !argIndexStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing location/fl+ln or 'callee_function_name' or 'arg_index'");
                    continue;
                }
                int argIndex = -1;
                try {
                    argIndex = std::stoi(argIndexStr->str());
                } catch (...) {
                    SVF::GraphReaderUtil::sendJsonError("invalid 'arg_index' value: " + argIndexStr->str());
                    continue;
                }
                // Parse optional offsets array
                std::vector<std::string> offsets;
                if (auto offsetsArray = cmd.getArray("offsets")) {
                    for (const auto& offsetVal : *offsetsArray) {
                        if (auto offsetStr = offsetVal.getAsString()) {
                            offsets.push_back(offsetStr->str());
                        } else {
                            SVF::GraphReaderUtil::sendJsonError("invalid offset value in 'offsets' array (must be strings)");
                            continue;
                        }
                    }
                }
                pq.findActualArgKeySVFGNodes(location, calleeFuncName->str(), argIndex, offsets);
            } else if (cname == "find-key-svfgnode-by-id") {
                auto idVal = cmd.getInteger("svfg_node_id");
                if (!idVal) {
                    sendStringError("missing 'svfg_node_id'");
                    continue;
                }
                if (*idVal < 0) {
                    sendStringError("invalid 'svfg_node_id' value: " + std::to_string(*idVal));
                    continue;
                }
                NodeID nodeId = static_cast<NodeID>(*idVal);

                std::vector<std::string> offsets;
                if (auto offsetsArray = cmd.getArray("offsets")) {
                    bool offsetsOk = true;
                    for (const auto& offsetVal : *offsetsArray) {
                        if (auto offsetStr = offsetVal.getAsString()) {
                            offsets.push_back(offsetStr->str());
                        } else if (auto offsetInt = offsetVal.getAsInteger()) {
                            offsets.push_back(std::to_string(*offsetInt));
                        } else {
                            sendStringError("invalid offset value in 'offsets' array (must be strings)");
                            offsetsOk = false;
                            break;
                        }
                    }
                    if (!offsetsOk) {
                        continue;
                    }
                }

                if (!svfg->hasSVFGNode(nodeId)) {
                    sendStringError("Cannot find key svfg nodes for id " + std::to_string(nodeId));
                    continue;
                }
                const SVFGNode* startSVFGNode = svfg->getSVFGNode(nodeId);
                if (!startSVFGNode) {
                    sendStringError("Cannot find key svfg nodes for id " + std::to_string(nodeId));
                    continue;
                }
                const FunObjVar* function = startSVFGNode->getFun();
                if (!function) {
                    sendStringError("Cannot find key svfg nodes for id " + std::to_string(nodeId));
                    continue;
                }
                // pq.setFindKeyByIdDebug(true); // debug
                pq.identifyKeySVFGNodesInFunction(function, startSVFGNode, false, offsets);
                // pq.setFindKeyByIdDebug(false); // debug
            } else if (cname == "find-call-arg-value-path-inside") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                auto calleeFuncName = cmd.getString("callee_function_name");
                auto argIndexStr = cmd.getString("arg_index");
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError) || !calleeFuncName || !argIndexStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing location/fl+ln or 'callee_function_name' or 'arg_index'");
                } else {
                    int argIndex = -1;
                    try {
                        argIndex = std::stoi(argIndexStr->str());
                    } catch (...) {
                        SVF::GraphReaderUtil::sendJsonError("invalid 'arg_index' value: " + argIndexStr->str());
                        continue;
                    }
                    const PAGNode* startPAGNode = SVF::GraphReaderUtil::getPAGNodeFromCallArg(
                        icfg, pag, location, argIndex, calleeFuncName->str());
                    if (!startPAGNode) {
                        SVF::GraphReaderUtil::sendJsonError(
                            "Cannot find PAGNode for CallArg at location '" + SVF::GraphReaderUtil::toString(location) +
                            "' with arg_index " + std::to_string(argIndex));
                        continue;
                    }
                    const SVFGNode* startSVFGNode = svfg->getDefSVFGNode(startPAGNode);
                    if (!startSVFGNode) {
                        SVF::GraphReaderUtil::sendJsonError(
                            "Cannot find SVFGNode for PAGNode " + std::to_string(startPAGNode->getId()));
                        continue;
                    }
                    std::vector<const SVFGNode*> startSVFGNodes{startSVFGNode};
                    pq.getValueSensitiveReturnInsidePath(location, startSVFGNodes);
                }
            } else if (cname == "analysis-lvar") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                auto eqPositionStr = cmd.getString("eq_position");
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError) || !eqPositionStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing location/fl+ln or 'eq_position'");
                    continue;
                }
                int eqPosition = -1;
                try {
                    eqPosition = std::stoi(eqPositionStr->str());
                } catch (...) {
                    SVF::GraphReaderUtil::sendJsonError("invalid 'eq_position' value: " + eqPositionStr->str());
                    continue;
                }
                llvm::json::Object analysisResult = SVF::GraphReaderUtil::analyzeStoreLValue(svfg, icfg, pag, location, eqPosition);
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(analysisResult))) << "\n";
                llvm::outs().flush();
            } else if (cname == "find-base-lvar-def") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                auto eqPositionStr = cmd.getString("eq_position");
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError) || !eqPositionStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing location/fl+ln or 'eq_position'");
                } else {
                    int eqPosition = -1;
                    try {
                        eqPosition = std::stoi(eqPositionStr->str());
                    } catch (...) {
                        SVF::GraphReaderUtil::sendJsonError("invalid 'eq_position' value: " + eqPositionStr->str());
                        continue;
                    }
                    // Step 1: Get PAGNode from LvarGEP
                    const PAGNode* targetPAG = SVF::GraphReaderUtil::getPAGNodeFromLvarGEP(icfg, pag, location, eqPosition);
                    if (!targetPAG) {
                        SVF::GraphReaderUtil::sendJsonError("Cannot find PAGNode for LvarGEP at location '" + SVF::GraphReaderUtil::toString(location) + "' with eq_position " + std::to_string(eqPosition));
                    } else {
                        // Step 2: Trace PAG store
                        SVF::GraphReaderUtil::tracePAGStore(svfg, pag, targetPAG);
                    }
                }
            } else if (cname == "check-return-pointer") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError)) {
                    SVF::GraphReaderUtil::sendJsonError(locationError);
                } else {
                    fq.checkReturnPointer(location);
                }
            } else if (cname == "check-always-return") {
                auto func = cmd.getString("function_name");
                if (!func) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'function_name'");
                } else {
                    fq.checkFunctionAlwaysReturn(func->str());
                }
            } else if (cname == "find-store-cl") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError)) {
                    SVF::GraphReaderUtil::sendJsonError(locationError);
                } else {
                    llvm::json::Object result = SVF::GraphReaderUtil::getStoreClInfoJson(svfg, icfg, location);
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                }
            } else if (cname == "find-gep-cl") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError)) {
                    SVF::GraphReaderUtil::sendJsonError(locationError);
                } else {
                    llvm::json::Object result = SVF::GraphReaderUtil::getGepClInfoJson(svfg, icfg, location);
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                }
            } else if (cname == "get-constrain-inside") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError)) {
                    if (auto typoLoc = cmd.getString("locaton")) {
                        size_t colonPos = typoLoc->str().find(':');
                        if (colonPos != std::string::npos) {
                            location.fl = typoLoc->str().substr(0, colonPos);
                            try {
                                std::string tail = typoLoc->str().substr(colonPos + 1);
                                size_t secondColonPos = tail.find(':');
                                if (secondColonPos != std::string::npos) {
                                    location.ln = std::stoll(tail.substr(0, secondColonPos));
                                    location.cl = std::stoll(tail.substr(secondColonPos + 1));
                                } else {
                                    location.ln = std::stoll(tail);
                                }
                            } catch (const std::exception&) {
                                location.ln = 0;
                            }
                        }
                    } else {
                        SVF::GraphReaderUtil::sendJsonError(locationError);
                        continue;
                    }
                }
                pq.getConstrainInside(location);
            } else if (cname == "show-code-line"){
                // DEBUG
                // 一个重要的debug功能 可以一直保留
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError)) {
                    SVF::GraphReaderUtil::sendJsonError(locationError);
                } else {
                    SVF::GraphReaderUtil::showCodeLineDebugInfo(svfg, icfg, SVF::GraphReaderUtil::toString(location));
                }
            } else if (cname == "list-formal-arg-nodes") {
                auto func = cmd.getString("function_name");
                llvm::json::Object result;
                if (!func) {
                    result["error"] = "missing 'function_name'";
                } else {
                    result = SVF::GraphReaderUtil::listFormalArgNodes(svfg, pag, func->str());
                }
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
            } else if (cname == "list-callsite-actual-arg-nodes") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                auto callee = cmd.getString("callee_function_name");
                llvm::json::Object result;
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError) || !callee) {
                    result["error"] = "missing location/fl+ln or 'callee_function_name'";
                } else {
                    result = SVF::GraphReaderUtil::listCallsiteActualArgNodes(svfg, icfg, location, callee->str());
                }
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
            } else if (cname == "find-callsite-return-node") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                auto callee = cmd.getString("callee_function_name");
                llvm::json::Object result;
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError) || !callee) {
                    result["error"] = "missing location/fl+ln or 'callee_function_name'";
                } else {
                    result = SVF::GraphReaderUtil::findCallsiteReturnNode(svfg, icfg, location, callee->str());
                }
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
            } else if (cname == "list-svfg-nodes-by-location") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                int64_t column = -1;
                llvm::json::Object result;
                if (!readStructuredLocation(cmd, "location", location, &column, locationError)) {
                    result["error"] = locationError;
                } else {
                    if (auto legacyColumn = cmd.getInteger("column")) {
                        column = *legacyColumn;
                    }
                    result = SVF::GraphReaderUtil::listSVFGNodesByLocation(svfg, icfg, location, column);
                }
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
            } else if (cname == "get-svfg-node-info") {
                auto idVal = cmd.getInteger("svfg_node_id");
                if (!idVal) {
                    sendStringError("missing 'svfg_node_id'");
                    continue;
                }
                if (*idVal < 0) {
                    sendStringError("invalid 'svfg_node_id' value: " + std::to_string(*idVal));
                    continue;
                }
                NodeID nodeId = static_cast<NodeID>(*idVal);
                if (!svfg->hasSVFGNode(nodeId)) {
                    llvm::json::Object result;
                    result["svfg_node"] = nullptr;
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                    llvm::outs().flush();
                    continue;
                }

                const SVFGNode* svfgNode = svfg->getSVFGNode(nodeId);
                if (!svfgNode) {
                    sendStringError("Cannot find SVFG node info for id " + std::to_string(nodeId));
                    continue;
                }

                llvm::json::Object nodeObj;
                nodeObj["svfg_node_id"] = static_cast<int64_t>(svfgNode->getId());
                nodeObj["node_type"] = SVF::GraphReaderUtil::getSVFGNodeKindString(svfgNode, true);
                std::string nodeDesc = svfgNode->toString();
                nodeObj["node_desc"] = nodeDesc;
                llvm::json::Object locObj = SVF::GraphReaderUtil::parseSourceLocation(nodeDesc);
                appendStructuredLocationFields(nodeObj, locObj);
                if (const ActualParmVFGNode* apNode = SVFUtil::dyn_cast<ActualParmVFGNode>(svfgNode)) {
                    const CallICFGNode* cs = apNode->getCallSite();
                    const PAGNode* param = apNode->getParam();
                    for (u32_t i = 0; i < cs->arg_size(); ++i) {
                        if (cs->getArgument(i) == param) {
                            nodeObj["arg_index"] = static_cast<int64_t>(i);
                            break;
                        }
                    }
                } else if (const FormalParmVFGNode* fpNode = SVFUtil::dyn_cast<FormalParmVFGNode>(svfgNode)) {
                    const FunEntryICFGNode* entry = icfg->getFunEntryICFGNode(fpNode->getFun());
                    if (entry) {
                        const auto& formalParms = entry->getFormalParms();
                        const PAGNode* param = fpNode->getParam();
                        for (size_t i = 0; i < formalParms.size(); ++i) {
                            if (formalParms[i] == param) {
                                nodeObj["arg_index"] = static_cast<int64_t>(i);
                                break;
                            }
                        }
                    }
                }

                llvm::json::Object result;
                result["svfg_node"] = std::move(nodeObj);
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
            } else if (cname == "find-all-free-caller") {
                llvm::json::Object result = SVF::GraphReaderUtil::findAllFreeCallers(pag);
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
            } else if (cname == "show-return-locations") {
                auto n = cmd.getString("name");
                if (!n) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'name'");
                } else {
                    std::string functionName = n->str();
                    const FunObjVar* function = pag->getFunObjVar(functionName);
                    if (!function) {
                        SVF::GraphReaderUtil::sendJsonError("Function '" + functionName + "' not found.");
                    } else {
                        // Find all return locations using the helper function
                        std::vector<const ICFGNode*> returnNodes = findActualReturnICFGNodes(icfg, function);
                        
                        // Build JSON output
                        llvm::json::Object result;
                        llvm::json::Array returnLocationsArray;
                        
                        for (const ICFGNode* node : returnNodes) {
                            llvm::json::Object locationObj;
                            
                            // Get node description
                            std::string nodeDesc = node->toString();
                            if (nodeDesc.empty()) {
                                nodeDesc = "Return location";
                            }
                            locationObj["node_desc"] = nodeDesc;
                            
                            // Get location from node description
                            llvm::json::Object locationInfo = SVF::GraphReaderUtil::parseSourceLocation(nodeDesc);
                            appendStructuredLocationFields(locationObj, locationInfo);
                            
                            returnLocationsArray.push_back(std::move(locationObj));
                        }
                        
                        result["return_locations"] = std::move(returnLocationsArray);
                        llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                        llvm::outs().flush();
                    }
                }
            } else if (cname == "macro-context-at-line") {
                SVF::GraphReaderUtil::SourceLocation location;
                std::string locationError;
                if (!readStructuredLocation(cmd, "location", location, nullptr, locationError)) {
                    SVF::GraphReaderUtil::sendJsonError(locationError);
                } else {
                    int64_t scanThrough = location.ln;
                    if (auto st = cmd.getInteger("scan_through_line")) {
                        scanThrough = *st;
                    }
                    bool includeHints = false;
                    if (auto ib = cmd.getBoolean("include_branch_hints")) {
                        includeHints = *ib;
                    }
                    std::map<std::string, std::string> macroDefs;
                    if (const llvm::json::Object* md = cmd.getObject("macro_defs")) {
                        for (const auto& kv : *md) {
                            std::string key = kv.first.str();
                            std::string val;
                            if (auto s = kv.second.getAsString()) {
                                val = s->str();
                            } else if (auto n = kv.second.getAsInteger()) {
                                val = std::to_string(*n);
                            } else if (auto b = kv.second.getAsBoolean()) {
                                val = *b ? "1" : "0";
                            }
                            macroDefs[key] = std::move(val);
                        }
                    }
                    llvm::json::Object result = SVF::GraphReaderUtil::macroContextAtLineJson(
                        location.fl, location.ln, scanThrough, includeHints, macroDefs);
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                    llvm::outs().flush();
                }
            } else if (cname == "exit") {
                shouldExit = true;
            } else {
                SVF::GraphReaderUtil::sendJsonError("unknown command: " + cname);
            }
        }

        llvm::outs().flush();
        llvm::outs().flush();
        if (shouldExit) break;
    }
}

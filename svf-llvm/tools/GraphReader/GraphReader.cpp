#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVFIR/SVFValue.h"
#include "WPA/Andersen.h"
#include "MSSA/MemSSA.h"
#include "Graphs/SVFG.h"
#include "Graphs/CallGraph.h"
#include "Graphs/ICFG.h"
#include "SABER/SaberCondAllocator.h"
#include "SABER/SaberCheckerAPI.h"
#include "Util/Options.h"
#include "Util/CDGBuilder.h"
// #include "GraphReader/SVFGChecker.h"
#include "GraphReaderSVFGBuilder.h"
#include "GraphReaderUtil.h"
#include "PathQuery.h"
#include "FunctionQuery.h"
#include <llvm/IR/DebugInfo.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <memory>
#include <unordered_set>

using namespace llvm;
using namespace SVF;
using namespace SVFUtil;

enum class Stage {
    PostPAG = 0,
    PostAndersen = 1,
    PostSVFG = 2
};

struct GraphReaderContext {
    SVFIR* pag = nullptr;
    ICFG* icfg = nullptr;
    SVFG* svfg = nullptr;
    FunctionQuery* fq = nullptr;
    PathQuery* pq = nullptr;
    SVF::GraphReaderUtil::ModelSpec* modelSpec = nullptr;
};

struct LoopResult {
    bool shouldExit = false;
    bool shouldContinue = false;
};

static const char* getStageName(Stage stage) {
    switch (stage) {
    case Stage::PostPAG:
        return "post-pag";
    case Stage::PostAndersen:
        return "post-andersen";
    case Stage::PostSVFG:
        return "post-svfg";
    }
    return "unknown";
}

static void sendStageReady(Stage stage) {
    llvm::json::Object ready;
    ready["ready"] = true;
    ready["stage"] = static_cast<int64_t>(stage);
    ready["stage_name"] = getStageName(stage);
    ready["message"] = "svf-stage-ready";
    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(ready))) << "\n";
    llvm::outs().flush();
}

static bool addUniqueName(std::vector<std::string>& vec, const std::string& name) {
    if (name.empty()) {
        return false;
    }
    for (const auto& item : vec) {
        if (item == name) {
            return false;
        }
    }
    vec.push_back(name);
    return true;
}

static bool addUniqueTarget(std::vector<SVF::GraphReaderUtil::ModelTarget>& vec,
                            const SVF::GraphReaderUtil::ModelTarget& target) {
    for (const auto& item : vec) {
        if (item.kind == target.kind && item.name == target.name && item.location == target.location) {
            return false;
        }
    }
    vec.push_back(target);
    return true;
}

static std::string gateCommand(Stage stage, const std::string& cname) {
    if (cname == "list-model-spec") {
        return "";
    }

    if (cname == "add-custom-api" || cname == "load-model-spec"
        || cname == "add-source-by-location" || cname == "add-sink-by-location") {
        if (stage != Stage::PostPAG) {
            return "command only available at stage post-pag";
        }
        return "";
    }

    static const std::unordered_set<std::string> stage1Allowed = {
        "find-function-body-by-name",
        "find-function-body-by-location",
        "find-all-function-call-sites",
        "find-all-function-callees",
        "find-cond-path",
        "find-cond-path-inside",
        "get-constrain-inside",
        "check-return-pointer",
        "check-always-return",
        "find-all-free-caller",
        "show-return-locations"
    };

    if (stage == Stage::PostPAG) {
        if (stage1Allowed.count(cname) != 0) {
            return "command not available before post-andersen";
        }
        return "command not available before post-svfg";
    }

    if (stage == Stage::PostAndersen) {
        if (stage1Allowed.count(cname) != 0) {
            return "";
        }
        return "command not available before post-svfg";
    }

    return "";
}

static LoopResult runCommandLoop(Stage stage, bool interleaving, GraphReaderContext& ctx) {
    while (true) {
        std::string jsonInput;
        if (!std::getline(std::cin, jsonInput)) {
            return LoopResult{true, false};
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
        bool shouldContinue = false;

        for (auto &cmd : cmds) {
            std::string cname;
            if (auto s = cmd.getString("command")) {
                cname = s->str();
            } else {
                SVF::GraphReaderUtil::sendJsonError("missing 'command'");
                continue;
            }

            if (cname == "exit") {
                shouldExit = true;
                break;
            }

            if (cname == "ping") {
                llvm::json::Object result;
                result["error"] = false;
                result["message"] = "pong";
                result["stage"] = static_cast<int64_t>(stage);
                result["stage_name"] = getStageName(stage);
                result["pag_ready"] = ctx.pag != nullptr;
                result["andersen_ready"] = ctx.icfg != nullptr;
                result["svfg_ready"] = ctx.svfg != nullptr;
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
                continue;
            }

            if (cname == "continue") {
                if (!interleaving) {
                    SVF::GraphReaderUtil::sendJsonError("interleaving not enabled");
                    continue;
                }
                if (stage == Stage::PostSVFG) {
                    SVF::GraphReaderUtil::sendJsonError("already at final stage");
                    continue;
                }
                shouldContinue = true;
                break;
            }

            std::string gateErr = gateCommand(stage, cname);
            if (!gateErr.empty()) {
                SVF::GraphReaderUtil::sendJsonError(gateErr);
                continue;
            }

            auto sendStringError = [](const std::string& message) {
                llvm::json::Object result;
                result["error"] = message;
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
            };

            if (cname == "list-model-spec") {
                llvm::json::Object result;
                result["error"] = false;
                if (ctx.modelSpec) {
                    result["spec"] = SVF::GraphReaderUtil::modelSpecToJson(*ctx.modelSpec);
                } else {
                    result["spec"] = llvm::json::Object{};
                }
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
                continue;
            }

            if (cname == "add-custom-api") {
                if (!ctx.modelSpec) {
                    SVF::GraphReaderUtil::sendJsonError("model spec not initialized");
                    continue;
                }
                auto kindVal = cmd.getString("kind");
                if (!kindVal) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'kind'");
                    continue;
                }
                std::string kind = kindVal->str();
                SaberCheckerAPI::CHECKER_TYPE type = SaberCheckerAPI::CK_DUMMY;
                std::vector<std::string>* apiVec = nullptr;
                if (kind == "alloc") {
                    type = SaberCheckerAPI::CK_ALLOC;
                    apiVec = &ctx.modelSpec->allocApis;
                } else if (kind == "free") {
                    type = SaberCheckerAPI::CK_FREE;
                    apiVec = &ctx.modelSpec->freeApis;
                } else if (kind == "fopen") {
                    type = SaberCheckerAPI::CK_FOPEN;
                    apiVec = &ctx.modelSpec->fopenApis;
                } else if (kind == "fclose") {
                    type = SaberCheckerAPI::CK_FCLOSE;
                    apiVec = &ctx.modelSpec->fcloseApis;
                } else {
                    SVF::GraphReaderUtil::sendJsonError("unknown kind: " + kind);
                    continue;
                }

                std::vector<std::string> names;
                if (auto nameVal = cmd.getString("name")) {
                    names.push_back(nameVal->str());
                }
                if (auto nameArray = cmd.getArray("names")) {
                    for (const auto& v : *nameArray) {
                        if (auto s = v.getAsString()) {
                            names.push_back(s->str());
                        } else {
                            SVF::GraphReaderUtil::sendJsonError("names must be string array");
                            names.clear();
                            break;
                        }
                    }
                }
                if (names.empty()) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'name' or 'names'");
                    continue;
                }

                llvm::json::Array added;
                SaberCheckerAPI* checkerAPI = SaberCheckerAPI::getCheckerAPI();
                bool ok = true;
                for (const auto& name : names) {
                    if (!addUniqueName(*apiVec, name)) {
                        continue;
                    }
                    if (!checkerAPI->addCustomAPI(name, type, true)) {
                        SVF::GraphReaderUtil::sendJsonError("failed to add custom api: " + name);
                        ok = false;
                        break;
                    }
                    added.push_back(name);
                }
                if (!ok) {
                    continue;
                }

                llvm::json::Object result;
                result["error"] = false;
                result["kind"] = kind;
                result["added"] = std::move(added);
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
                continue;
            }

            if (cname == "load-model-spec") {
                if (!ctx.modelSpec) {
                    SVF::GraphReaderUtil::sendJsonError("model spec not initialized");
                    continue;
                }
                std::string mode = "replace";
                if (auto modeVal = cmd.getString("mode")) {
                    mode = modeVal->str();
                }
                if (mode != "replace" && mode != "merge") {
                    SVF::GraphReaderUtil::sendJsonError("invalid mode: " + mode);
                    continue;
                }

                llvm::json::Object specObj;
                bool hasSpec = false;
                if (auto specVal = cmd.getObject("spec")) {
                    specObj = *specVal;
                    hasSpec = true;
                } else if (auto pathVal = cmd.getString("path")) {
                    auto bufferOrErr = llvm::MemoryBuffer::getFile(pathVal->str());
                    if (!bufferOrErr) {
                        SVF::GraphReaderUtil::sendJsonError("cannot read file: " + pathVal->str());
                        continue;
                    }
                    auto parsed = llvm::json::parse(bufferOrErr.get()->getBuffer());
                    if (!parsed) {
                        SVF::GraphReaderUtil::sendJsonError("json parse error in model spec");
                        continue;
                    }
                    llvm::json::Value root = std::move(*parsed);
                    if (auto* obj = root.getAsObject()) {
                        specObj = *obj;
                        hasSpec = true;
                    } else {
                        SVF::GraphReaderUtil::sendJsonError("model spec root must be an object");
                        continue;
                    }
                }

                if (!hasSpec) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'spec' or 'path'");
                    continue;
                }

                std::string err;
                bool merge = (mode == "merge");
                if (!SVF::GraphReaderUtil::parseModelSpecObject(specObj, *ctx.modelSpec, err, merge)) {
                    SVF::GraphReaderUtil::sendJsonError("model spec parse error: " + err);
                    continue;
                }
                if (!SVF::GraphReaderUtil::applyModelSpecToSaber(*ctx.modelSpec, true, err)) {
                    SVF::GraphReaderUtil::sendJsonError("model spec apply error: " + err);
                    continue;
                }

                llvm::json::Object result;
                result["error"] = false;
                result["mode"] = mode;
                result["spec"] = SVF::GraphReaderUtil::modelSpecToJson(*ctx.modelSpec);
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
                continue;
            }

            if (cname == "add-source-by-location" || cname == "add-sink-by-location") {
                if (!ctx.modelSpec) {
                    SVF::GraphReaderUtil::sendJsonError("model spec not initialized");
                    continue;
                }
                auto locVal = cmd.getString("location");
                if (!locVal) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location'");
                    continue;
                }
                SVF::GraphReaderUtil::ModelTarget target;
                target.kind = "location";
                target.location = locVal->str();

                bool added = false;
                if (cname == "add-source-by-location") {
                    added = addUniqueTarget(ctx.modelSpec->sources, target);
                } else {
                    added = addUniqueTarget(ctx.modelSpec->sinks, target);
                }

                llvm::json::Object result;
                result["error"] = false;
                result["added"] = added;
                result["location"] = target.location;
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
                continue;
            }

            // function TODOs: 
            // 1. 在某一个赋值语句处 构建一个针对左值的GEP操作栈
            // 2. 在所有find value path index系列中 将GEP栈作为一个可选的传入变量 
            // 我们能知道目前追踪的是什么左值 真正持有内存的是哪个成员变量 
            // 3. 在所有find value path index系列中 将所有icfg不可达到返回位置也加入
            // 原因是 icfg上的遍历在面对循环时可能出现将可到达的位置判定为不可达 从而导致我们漏掉一些路径
            if (cname == "find-function-body-by-name") {
                if (auto n = cmd.getString("name")) {
                    if (ctx.fq) {
                        ctx.fq->findFunctionBodyByName(n->str());
                    } else {
                        SVF::GraphReaderUtil::sendJsonError("FunctionQuery not initialized");
                    }
                } else {
                    SVF::GraphReaderUtil::sendJsonError("missing 'name'");
                }
            } else if (cname == "find-function-body-by-location") {
                if (auto loc = cmd.getString("location")) {
                    if (ctx.fq) {
                        ctx.fq->findFunctionBodyByLocation(loc->str());
                    } else {
                        SVF::GraphReaderUtil::sendJsonError("FunctionQuery not initialized");
                    }
                } else {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location'");
                }
            } else if (cname == "find-all-function-call-sites") {
                if (auto n = cmd.getString("name")) {
                    if (ctx.fq) {
                        ctx.fq->findCallSites(n->str());
                    } else {
                        SVF::GraphReaderUtil::sendJsonError("FunctionQuery not initialized");
                    }
                } else {
                    SVF::GraphReaderUtil::sendJsonError("missing 'name'");
                }
            } else if (cname == "find-all-function-callees") {
                if (auto n = cmd.getString("name")) {
                    if (ctx.fq) {
                        ctx.fq->findAllCalleesByName(n->str());
                    } else {
                        SVF::GraphReaderUtil::sendJsonError("FunctionQuery not initialized");
                    }
                } else {
                    SVF::GraphReaderUtil::sendJsonError("missing 'name'");
                }
            } else if (cname == "find-cond-path") {
                auto s = cmd.getString("start");
                auto e = cmd.getString("end");
                if (!s || !e) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'start' or 'end'");
                } else if (ctx.pq) {
                    ctx.pq->getConditionPath(s->str(), e->str());
                } else {
                    SVF::GraphReaderUtil::sendJsonError("PathQuery not initialized");
                }
            } else if (cname == "find-cond-path-inside") {
                auto s = cmd.getString("start");
                auto e = cmd.getString("end");
                if (!s || !e) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'start' or 'end'");
                } else if (ctx.pq) {
                    ctx.pq->getConditionInsidePath(s->str(), e->str());
                } else {
                    SVF::GraphReaderUtil::sendJsonError("PathQuery not initialized");
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
                if (!SVF::GraphReaderUtil::fetchFunctionStartLocation(ctx.pag, functionNameStr, startLocation)) {
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
                const PAGNode* startPAGNode = SVF::GraphReaderUtil::getPAGNodeFromArg(ctx.pag, functionNameStr, argIndex);
                if (startPAGNode && ctx.svfg) {
                    const SVFGNode* startSVFGNode = ctx.svfg->getDefSVFGNode(startPAGNode);
                    if (startSVFGNode) {
                        startSVFGNodes.push_back(startSVFGNode);
                    }
                }
                if (ctx.pq) {
                    ctx.pq->getValueSensitiveReturnInsidePath(startLocation, startSVFGNodes);
                } else {
                    SVF::GraphReaderUtil::sendJsonError("PathQuery not initialized");
                }
            } else if (cname == "find-lvalue-path-inside") {
                auto loc = cmd.getString("location");
                auto eqPositionStr = cmd.getString("eq_position");
                if (!loc || !eqPositionStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location' or 'eq_position'");
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
                        const PAGNode* startPAGNode = SVF::GraphReaderUtil::getPAGNodeFromLvar(ctx.icfg, ctx.pag, loc->str(), eqPosition);
                        if (!startPAGNode) {
                            SVF::GraphReaderUtil::sendJsonError("Cannot find PAGNode for Lvar at location '" + loc->str() + "' with eq_position " + std::to_string(eqPosition));
                            continue;
                        }
                        if (!ctx.svfg) {
                            SVF::GraphReaderUtil::sendJsonError("SVFG not initialized");
                            continue;
                        }
                        const SVFGNode* startSVFGNode = ctx.svfg->getDefSVFGNode(startPAGNode);
                        if (!startSVFGNode) {
                            SVF::GraphReaderUtil::sendJsonError("Cannot find SVFGNode for PAGNode " + std::to_string(startPAGNode->getId()));
                            continue;
                        }
                        startSVFGNodes.push_back(startSVFGNode);
                    }
                    if (ctx.pq) {
                        ctx.pq->getValueSensitiveReturnInsidePath(loc->str(), startSVFGNodes);
                    } else {
                        SVF::GraphReaderUtil::sendJsonError("PathQuery not initialized");
                    }
                }
            } else if (cname == "find-lvalue-detail-path-inside") {
                auto loc = cmd.getString("location");
                auto eqPositionStr = cmd.getString("eq_position");
                if (!loc || !eqPositionStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location' or 'eq_position'");
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
                        const PAGNode* startPAGNode = SVF::GraphReaderUtil::getPAGNodeFromLvar(ctx.icfg, ctx.pag, loc->str(), eqPosition);
                        if (!startPAGNode) {
                            SVF::GraphReaderUtil::sendJsonError("Cannot find PAGNode for Lvar at location '" + loc->str() + "' with eq_position " + std::to_string(eqPosition));
                            continue;
                        }
                        if (!ctx.svfg) {
                            SVF::GraphReaderUtil::sendJsonError("SVFG not initialized");
                            continue;
                        }
                        const SVFGNode* startSVFGNode = ctx.svfg->getDefSVFGNode(startPAGNode);
                        if (!startSVFGNode) {
                            SVF::GraphReaderUtil::sendJsonError("Cannot find SVFGNode for PAGNode " + std::to_string(startPAGNode->getId()));
                            continue;
                        }
                        startSVFGNodes.push_back(startSVFGNode);
                    }
                    if (ctx.pq) {
                        ctx.pq->getValueSensitiveReturnInsidePathDetailed(loc->str(), startSVFGNodes);
                    } else {
                        SVF::GraphReaderUtil::sendJsonError("PathQuery not initialized");
                    }
                }
            } else if (cname == "find-lvalue-detail-path-inside-store") {
                auto loc = cmd.getString("location");
                auto eqPositionStr = cmd.getString("eq_position");
                if (!loc || !eqPositionStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location' or 'eq_position'");
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
                std::vector<const ICFGNode*> nodes = SVF::GraphReaderUtil::findAllICFGNodesByLocation(ctx.icfg, loc->str());
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
                for (auto& pair : *ctx.svfg) {
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
                ctx.pq->getValueSensitiveReturnInsidePathDetailed(loc->str(), startSVFGNodes);
            } else if (cname == "find-lvalue-key_svfgnode") {
                auto loc = cmd.getString("location");
                auto eqPositionStr = cmd.getString("eq_position");
                if (!loc || !eqPositionStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location' or 'eq_position'");
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
                ctx.pq->findLvalueKeySVFGNodes(loc->str(), eqPosition, offsets);
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
                ctx.pq->findFormalArgKeySVFGNodes(functionName->str(), argIndex, offsets);
            } else if (cname == "find-actual_arg-key_svfgnode") {
                auto loc = cmd.getString("location");
                auto calleeFuncName = cmd.getString("callee_function_name");
                auto argIndexStr = cmd.getString("arg_index");
                if (!loc || !calleeFuncName || !argIndexStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location' or 'callee_function_name' or 'arg_index'");
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
                ctx.pq->findActualArgKeySVFGNodes(loc->str(), calleeFuncName->str(), argIndex, offsets);
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

                if (!ctx.svfg->hasSVFGNode(nodeId)) {
                    sendStringError("Cannot find key svfg nodes for id " + std::to_string(nodeId));
                    continue;
                }
                const SVFGNode* startSVFGNode = ctx.svfg->getSVFGNode(nodeId);
                if (!startSVFGNode) {
                    sendStringError("Cannot find key svfg nodes for id " + std::to_string(nodeId));
                    continue;
                }
                const FunObjVar* function = startSVFGNode->getFun();
                if (!function) {
                    sendStringError("Cannot find key svfg nodes for id " + std::to_string(nodeId));
                    continue;
                }
                ctx.pq->identifyKeySVFGNodesInFunction(function, startSVFGNode, false, offsets);
            } else if (cname == "find-call-arg-value-path-inside") {
                auto loc = cmd.getString("location");
                auto calleeFuncName = cmd.getString("callee_function_name");
                auto argIndexStr = cmd.getString("arg_index");
                if (!loc || !calleeFuncName || !argIndexStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location' or 'callee_function_name' or 'arg_index'");
                } else {
                    int argIndex = -1;
                    try {
                        argIndex = std::stoi(argIndexStr->str());
                    } catch (...) {
                        SVF::GraphReaderUtil::sendJsonError("invalid 'arg_index' value: " + argIndexStr->str());
                        continue;
                    }
                    const PAGNode* startPAGNode = SVF::GraphReaderUtil::getPAGNodeFromCallArg(
                        ctx.icfg, ctx.pag, loc->str(), argIndex, calleeFuncName->str());
                    if (!startPAGNode) {
                        SVF::GraphReaderUtil::sendJsonError(
                            "Cannot find PAGNode for CallArg at location '" + loc->str() +
                            "' with arg_index " + std::to_string(argIndex));
                        continue;
                    }
                    const SVFGNode* startSVFGNode = ctx.svfg->getDefSVFGNode(startPAGNode);
                    if (!startSVFGNode) {
                        SVF::GraphReaderUtil::sendJsonError(
                            "Cannot find SVFGNode for PAGNode " + std::to_string(startPAGNode->getId()));
                        continue;
                    }
                    std::vector<const SVFGNode*> startSVFGNodes{startSVFGNode};
                    ctx.pq->getValueSensitiveReturnInsidePath(loc->str(), startSVFGNodes);
                }
            } else if (cname == "analysis-lvar") {
                auto loc = cmd.getString("location");
                auto eqPositionStr = cmd.getString("eq_position");
                if (!loc || !eqPositionStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location' or 'eq_position'");
                } else {
                    int eqPosition = -1;
                    try {
                        eqPosition = std::stoi(eqPositionStr->str());
                    } catch (...) {
                        SVF::GraphReaderUtil::sendJsonError("invalid 'eq_position' value: " + eqPositionStr->str());
                        continue;
                    }
                    if (!ctx.svfg) {
                        SVF::GraphReaderUtil::sendJsonError("SVFG not initialized");
                        continue;
                    }
                    llvm::json::Object result = SVF::GraphReaderUtil::analyzeStoreLValue(ctx.svfg, ctx.icfg, ctx.pag, loc->str(), eqPosition);
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                    llvm::outs().flush();
                }
            } else if (cname == "find-base-lvar-def") {
                auto loc = cmd.getString("location");
                auto eqPositionStr = cmd.getString("eq_position");
                if (!loc || !eqPositionStr) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location' or 'eq_position'");
                } else {
                    int eqPosition = -1;
                    try {
                        eqPosition = std::stoi(eqPositionStr->str());
                    } catch (...) {
                        SVF::GraphReaderUtil::sendJsonError("invalid 'eq_position' value: " + eqPositionStr->str());
                        continue;
                    }
                    if (!ctx.svfg) {
                        SVF::GraphReaderUtil::sendJsonError("SVFG not initialized");
                        continue;
                    }
                    llvm::json::Object result = SVF::GraphReaderUtil::findBaseLvarDef(ctx.svfg, ctx.icfg, ctx.pag, loc->str(), eqPosition);
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                    llvm::outs().flush();
                }
            } else if (cname == "check-return-pointer") {
                auto loc = cmd.getString("location");
                if (!loc) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location'");
                } else if (ctx.fq) {
                    ctx.fq->checkReturnPointer(loc->str());
                } else {
                    SVF::GraphReaderUtil::sendJsonError("FunctionQuery not initialized");
                }
            } else if (cname == "check-always-return") {
                auto n = cmd.getString("name");
                if (!n) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'name'");
                } else if (ctx.fq) {
                    ctx.fq->checkFunctionAlwaysReturn(n->str());
                } else {
                    SVF::GraphReaderUtil::sendJsonError("FunctionQuery not initialized");
                }
            } else if (cname == "find-store-cl") {
                auto loc = cmd.getString("location");
                if (!loc) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location'");
                } else if (ctx.svfg) {
                    llvm::json::Object result = SVF::GraphReaderUtil::getStoreClInfoJson(ctx.svfg, ctx.icfg, loc->str());
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                    llvm::outs().flush();
                } else {
                    SVF::GraphReaderUtil::sendJsonError("SVFG not initialized");
                }
            } else if (cname == "find-gep-cl") {
                auto loc = cmd.getString("location");
                if (!loc) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location'");
                } else if (ctx.svfg) {
                    llvm::json::Object result = SVF::GraphReaderUtil::getGepClInfoJson(ctx.svfg, ctx.icfg, loc->str());
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                    llvm::outs().flush();
                } else {
                    SVF::GraphReaderUtil::sendJsonError("SVFG not initialized");
                }
            } else if (cname == "get-constrain-inside") {
                auto loc = cmd.getString("location");
                if (!loc) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location'");
                } else if (ctx.pq) {
                    ctx.pq->getConstrainInside(loc->str());
                } else {
                    SVF::GraphReaderUtil::sendJsonError("PathQuery not initialized");
                }
            } else if (cname == "show-code-line") {
                auto loc = cmd.getString("location");
                if (!loc) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location'");
                } else if (ctx.svfg) {
                    SVF::GraphReaderUtil::showCodeLineDebugInfo(ctx.svfg, ctx.icfg, loc->str());
                } else {
                    SVF::GraphReaderUtil::sendJsonError("SVFG not initialized");
                }
            } else if (cname == "list-formal-arg-nodes") {
                auto n = cmd.getString("function_name");
                if (!n) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'function_name'");
                } else if (ctx.svfg) {
                    llvm::json::Object result = SVF::GraphReaderUtil::listFormalArgNodes(ctx.svfg, ctx.pag, n->str());
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                    llvm::outs().flush();
                } else {
                    SVF::GraphReaderUtil::sendJsonError("SVFG not initialized");
                }
            } else if (cname == "list-callsite-actual-arg-nodes") {
                auto loc = cmd.getString("location");
                auto calleeFunctionName = cmd.getString("callee_function_name");
                if (!loc || !calleeFunctionName) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location' or 'callee_function_name'");
                } else if (ctx.svfg) {
                    llvm::json::Object result = SVF::GraphReaderUtil::listCallsiteActualArgNodes(ctx.svfg, ctx.icfg, loc->str(), calleeFunctionName->str());
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                    llvm::outs().flush();
                } else {
                    SVF::GraphReaderUtil::sendJsonError("SVFG not initialized");
                }
            } else if (cname == "find-callsite-return-node") {
                auto loc = cmd.getString("location");
                auto calleeFunctionName = cmd.getString("callee_function_name");
                if (!loc || !calleeFunctionName) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location' or 'callee_function_name'");
                } else if (ctx.svfg) {
                    llvm::json::Object result = SVF::GraphReaderUtil::findCallsiteReturnNode(ctx.svfg, ctx.icfg, loc->str(), calleeFunctionName->str());
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                    llvm::outs().flush();
                } else {
                    SVF::GraphReaderUtil::sendJsonError("SVFG not initialized");
                }
            } else if (cname == "list-svfg-nodes-by-location") {
                auto loc = cmd.getString("location");
                auto col = cmd.getInteger("column");
                llvm::json::Object result;
                if (!loc) {
                    result["error"] = "missing 'location'";
                } else {
                    int64_t column = -1;
                    if (col) {
                        column = *col;
                    }
                    result = SVF::GraphReaderUtil::listSVFGNodesByLocation(ctx.svfg, ctx.icfg, loc->str(), column);
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
                if (!ctx.svfg->hasSVFGNode(nodeId)) {
                    llvm::json::Object result;
                    result["svfg_node"] = nullptr;
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                    llvm::outs().flush();
                    continue;
                }

                const SVFGNode* svfgNode = ctx.svfg->getSVFGNode(nodeId);
                if (!svfgNode) {
                    sendStringError("Cannot find SVFG node info for id " + std::to_string(nodeId));
                    continue;
                }

                auto formatLocationString = [](const llvm::json::Object& locObj) -> std::string {
                    std::string filename;
                    int64_t line = 0;

                    if (auto fl = locObj.getString("fl")) {
                        filename = fl->str();
                    }
                    if (auto ln = locObj.getInteger("ln")) {
                        line = *ln;
                    }

                    if (filename.empty() && line == 0) {
                        return "";
                    } else if (filename.empty()) {
                        return std::to_string(line);
                    } else if (line == 0) {
                        return filename;
                    } else {
                        return filename + ":" + std::to_string(line);
                    }
                };

                llvm::json::Object nodeObj;
                nodeObj["svfg_node_id"] = static_cast<int64_t>(svfgNode->getId());
                nodeObj["node_type"] = SVF::GraphReaderUtil::getSVFGNodeKindString(svfgNode, true);
                std::string nodeDesc = svfgNode->toString();
                nodeObj["node_desc"] = nodeDesc;
                llvm::json::Object locObj = SVF::GraphReaderUtil::parseSourceLocation(nodeDesc);
                nodeObj["location"] = formatLocationString(locObj);

                llvm::json::Object result;
                result["svfg_node"] = std::move(nodeObj);
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
            } else if (cname == "find-all-free-caller") {
                llvm::json::Object result = SVF::GraphReaderUtil::findAllFreeCallers(ctx.pag);
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                llvm::outs().flush();
            } else if (cname == "show-return-locations") {
                auto n = cmd.getString("name");
                if (!n) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'name'");
                } else {
                    std::string functionName = n->str();
                    const FunObjVar* function = ctx.pag->getFunObjVar(functionName);
                    if (!function) {
                        SVF::GraphReaderUtil::sendJsonError("Function '" + functionName + "' not found.");
                    } else {
                        // Find all return locations using the helper function
                        std::vector<const ICFGNode*> returnNodes = findActualReturnICFGNodes(ctx.icfg, function);

                        // Helper function to format location JSON object to "filename:line" string
                        auto formatLocationString = [](const llvm::json::Object& locObj) -> std::string {
                            std::string filename;
                            int64_t line = 0;

                            if (auto fl = locObj.getString("fl")) {
                                filename = fl->str();
                            }
                            if (auto ln = locObj.getInteger("ln")) {
                                line = *ln;
                            }

                            if (filename.empty() && line == 0) {
                                return "";
                            } else if (filename.empty()) {
                                return std::to_string(line);
                            } else if (line == 0) {
                                return filename;
                            } else {
                                return filename + ":" + std::to_string(line);
                            }
                        };

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
                            locationObj["location"] = formatLocationString(locationInfo);

                            returnLocationsArray.push_back(std::move(locationObj));
                        }

                        result["return_locations"] = std::move(returnLocationsArray);
                        llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                        llvm::outs().flush();
                    }
                }
            } else {
                SVF::GraphReaderUtil::sendJsonError("unknown command: " + cname);
            }
        }

        llvm::outs().flush();
        llvm::outs().flush();
        if (shouldExit) {
            return LoopResult{true, false};
        }
        if (shouldContinue) {
            return LoopResult{false, true};
        }
    }
}

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

    bool interleaving = Options::Interleaving();
    GraphReaderUtil::ModelSpec modelSpec;
    GraphReaderContext ctx;
    ctx.pag = pag;
    ctx.modelSpec = &modelSpec;

    if (interleaving) {
        sendStageReady(Stage::PostPAG);
        LoopResult result = runCommandLoop(Stage::PostPAG, true, ctx);
        if (result.shouldExit) {
            return 0;
        }
    }

    AndersenWaveDiff* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    ICFG* icfg = ander->getICFG();

    FunctionQuery fqStage1(icfg, pag, nullptr);
    PathQuery pqStage1(nullptr, icfg);
    ctx.icfg = icfg;
    ctx.fq = &fqStage1;
    ctx.pq = &pqStage1;

    if (interleaving) {
        sendStageReady(Stage::PostAndersen);
        LoopResult result = runCommandLoop(Stage::PostAndersen, true, ctx);
        if (result.shouldExit) {
            return 0;
        }
    }

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
    ctx.svfg = svfg;
    ctx.fq = &fq;
    ctx.pq = &pq;

    if (interleaving) {
        sendStageReady(Stage::PostSVFG);
        runCommandLoop(Stage::PostSVFG, true, ctx);
        return 0;
    }

    {
        llvm::json::Object ready;
        ready["ready"] = true;
        ready["message"] = "graphreader-initialized";
        llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(ready))) << "\n";
        llvm::outs().flush();
    }

    runCommandLoop(Stage::PostSVFG, false, ctx);
    return 0;
}


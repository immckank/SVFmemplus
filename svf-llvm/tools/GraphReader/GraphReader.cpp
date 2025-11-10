#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVFIR/SVFValue.h"
#include "WPA/Andersen.h"
#include "MSSA/MemSSA.h"
#include "Graphs/SVFG.h"
#include "Graphs/CallGraph.h"
#include "Graphs/ICFG.h"
#include "Util/Options.h"
#include "Util/CDGBuilder.h"
// #include "GraphReader/SVFGChecker.h"
#include "GraphReaderSVFGBuilder.h"
#include "GraphReaderUtil.h"
#include "PathQuery.h"
#include "FunctionQuery.h"
#include <llvm/IR/DebugInfo.h>
#include <llvm/Support/JSON.h>

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
                if (auto loc = cmd.getString("location")) {
                    fq.findFunctionBodyByLocation(loc->str());
                } else {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location'");
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
                auto s = cmd.getString("start");
                auto e = cmd.getString("end");
                if (!s || !e) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'start' or 'end'");
                } else {
                    pq.getConditionPath(s->str(), e->str());
                }
            } else if (cname == "find-cond-path-inside") {
                auto s = cmd.getString("start");
                auto e = cmd.getString("end");
                if (!s || !e) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'start' or 'end'");
                } else {
                    pq.getConditionInsidePath(s->str(), e->str());
                }
            } else if (cname == "exit") {
                shouldExit = true;
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
                const PAGNode* startPAGNode = SVF::GraphReaderUtil::getPAGNodeFromArg(pag, functionNameStr, argIndex);
                    if (!startPAGNode) {
                    SVF::GraphReaderUtil::sendJsonError("Cannot find PAGNode for function '" + functionNameStr + "' argument " + std::to_string(argIndex));
                    continue;
                }
                const SVFGNode* startSVFGNode = svfg->getDefSVFGNode(startPAGNode);
                if (!startSVFGNode) {
                    SVF::GraphReaderUtil::sendJsonError("Cannot find SVFGNode for PAGNode " + std::to_string(startPAGNode->getId()));
                    continue;
                }
                std::vector<const SVFGNode*> startSVFGNodes{startSVFGNode};
                pq.getValueSensitiveReturnInsidePath(startLocation, startSVFGNodes);
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
                    const PAGNode* startPAGNode = SVF::GraphReaderUtil::getPAGNodeFromLvar(icfg, pag, loc->str(), eqPosition);
                    if (!startPAGNode) {
                        SVF::GraphReaderUtil::sendJsonError("Cannot find PAGNode for Lvar at location '" + loc->str() + "' with eq_position " + std::to_string(eqPosition));
                        continue;
                    }
                    const SVFGNode* startSVFGNode = svfg->getDefSVFGNode(startPAGNode);
                    if (!startSVFGNode) {
                        SVF::GraphReaderUtil::sendJsonError("Cannot find SVFGNode for PAGNode " + std::to_string(startPAGNode->getId()));
                        continue;
                    }
                    std::vector<const SVFGNode*> startSVFGNodes{startSVFGNode};
                    pq.getValueSensitiveReturnInsidePath(loc->str(), startSVFGNodes);
                }
            } else if (cname == "analysis-lvar") {
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
                llvm::json::Object analysisResult = SVF::GraphReaderUtil::analyzeStoreLValue(svfg, icfg, pag, loc->str(), eqPosition);
                llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(analysisResult))) << "\n";
                llvm::outs().flush();
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
                        icfg, pag, loc->str(), argIndex, calleeFuncName->str());
                    if (!startPAGNode) {
                        SVF::GraphReaderUtil::sendJsonError(
                            "Cannot find PAGNode for CallArg at location '" + loc->str() +
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
                    pq.getValueSensitiveReturnInsidePath(loc->str(), startSVFGNodes);
                }
            } else if (cname == "check-return-pointer") {
                auto loc = cmd.getString("location");
                if (!loc) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location'");
                } else {
                    fq.checkReturnPointer(loc->str());
                }
            } else if (cname == "find-store-cl") {
                auto loc = cmd.getString("location");
                if (!loc) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location'");
                } else {
                    llvm::json::Object result = SVF::GraphReaderUtil::getStoreClInfoJson(svfg, icfg, loc->str());
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
                }
            } else if (cname == "find-gep-cl") {
                auto loc = cmd.getString("location");
                if (!loc) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location'");
                } else {
                    llvm::json::Object result = SVF::GraphReaderUtil::getGepClInfoJson(svfg, icfg, loc->str());
                    llvm::outs() << llvm::formatv("{0}", llvm::json::Value(std::move(result))) << "\n";
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
                    // Step 1: Get PAGNode from LvarGEP
                    const PAGNode* targetPAG = SVF::GraphReaderUtil::getPAGNodeFromLvarGEP(icfg, pag, loc->str(), eqPosition);
                    if (!targetPAG) {
                        SVF::GraphReaderUtil::sendJsonError("Cannot find PAGNode for LvarGEP at location '" + loc->str() + "' with eq_position " + std::to_string(eqPosition));
                    } else {
                        // Step 2: Trace PAG store
                        SVF::GraphReaderUtil::tracePAGStore(svfg, pag, targetPAG);
                    }
                }
            } else if (cname == "show-code-line"){
                // DEBUG
                // 一个重要的debug功能 可以一直保留
                auto loc = cmd.getString("location");
                if (!loc) {
                    SVF::GraphReaderUtil::sendJsonError("missing 'location'");
                } else {
                    SVF::GraphReaderUtil::showCodeLineDebugInfo(svfg, icfg, loc->str());
                }
            }else {
                SVF::GraphReaderUtil::sendJsonError("unknown command: " + cname);
            }
        }

        llvm::outs().flush();
        llvm::outs().flush();
        if (shouldExit) break;
    }
}

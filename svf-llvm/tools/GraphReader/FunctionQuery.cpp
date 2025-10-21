#include "FunctionQuery.h"
#include "GraphReaderUtil.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVFIR/SVFValue.h"
#include "SVF-LLVM/LLVMModule.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>

using namespace llvm;
using namespace SVF;
using namespace SVFUtil;

SVF::FunctionQuery::FunctionQuery(ICFG* i, SVFIR* p) : icfg(i), pag(p) {}

void SVF::FunctionQuery::findCallSites(const std::string& functionName) {
    llvm::json::Object result;
    llvm::json::Array callSites;
    Set<const ICFGNode*> reportedCallSites;

    for (ICFG::const_iterator it = icfg->begin(), eit = icfg->end(); it != eit; ++it) {
        const ICFGNode* node = it->second;
        if (const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node)) {
            if (reportedCallSites.count(callNode)) {
                continue;
            }
            for (ICFGEdge* edge : callNode->getOutEdges()) {
                if (SVFUtil::isa<CallCFGEdge>(edge)) {
                    const ICFGNode* calleeEntryNode = edge->getDstNode();
                    const FunObjVar* svfFun = calleeEntryNode->getFun();
                    if (svfFun && svfFun->getName() == functionName) {
                        llvm::json::Object site;
                        std::string locString = callNode->getSourceLoc();
                        std::string formattedLoc = "unknown";

                        llvm::json::Object locInfo = GraphReaderUtil::parseSourceLocation(locString);
                        if (auto file = locInfo.getString("fl")) {
                            if (auto line = locInfo.getInteger("ln")) {
                                formattedLoc = file->str() + ":" + std::to_string(*line);
                            }
                        }
                        site["location"] = formattedLoc;
                        callSites.push_back(std::move(site));
                        reportedCallSites.insert(callNode);
                        break;
                    }
                }
            }
        }
    }
    result["call_sites"] = std::move(callSites);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
}

void SVF::FunctionQuery::findCalleeBodyByLocation(const std::string& location) {
    llvm::json::Object result;
    llvm::json::Array calleeFunctions;

    const ICFGNode* node = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
    if (!node) {
        GraphReaderUtil::sendJsonError("Could not find ICFGNode for the given location.");
        return;
    }

    const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(node);
    if (!callNode) {
        GraphReaderUtil::sendJsonError("Node at the given location is not a function call site.");
        return;
    }

    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
    for (ICFGEdge* edge : callNode->getOutEdges()) {
        if (SVFUtil::isa<CallCFGEdge>(edge)) {
            const ICFGNode* calleeEntryNode = edge->getDstNode();
            const FunObjVar* svfFun = calleeEntryNode->getFun();
            if (svfFun) {
                const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(svfFun);
                const llvm::Function* llvmFun = SVFUtil::dyn_cast<llvm::Function>(llvmVal);
                calleeFunctions.push_back(GraphReaderUtil::getFunctionInfoJson(llvmFun));
            }
        }
    }
    result["callee_functions"] = std::move(calleeFunctions);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
}

void SVF::FunctionQuery::findFunctionBodyByLocation(const std::string& location) {
    llvm::json::Object result;
    const ICFGNode* node = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
    if (!node) {
        GraphReaderUtil::sendJsonError("Could not find ICFGNode for the given location.");
        return;
    }

    const FunObjVar* svfFun = node->getFun();
    if (!svfFun) {
        GraphReaderUtil::sendJsonError("The ICFGNode at the given location is not inside a function.");
        return;
    }

    const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(svfFun);
    const llvm::Function* llvmFun = SVFUtil::dyn_cast<llvm::Function>(llvmVal);

    result = GraphReaderUtil::getFunctionInfoJson(llvmFun);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
}

void SVF::FunctionQuery::findFunctionBodyByName(const std::string& functionName) {
    llvm::json::Object result;
    const FunObjVar* svfFun = pag->getFunObjVar(functionName);

    if (!svfFun) {
        GraphReaderUtil::sendJsonError("Function '" + functionName + "' not found.");
        // 调试输出：当找不到函数时，打印 PAG 中所有可用的函数名
        SVF::SVFUtil::errs() << "Debug: Available functions in PAG:\n";
        // 遍历 funArgsListMap 来获取所有的 FunObjVar，然后打印它们的名称
        for (auto const& [fun, args] : pag->getFunArgsMap()) {
            SVF::SVFUtil::errs() << "  - " << fun->getName() << "\n";
        }
        return;
    }

    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
    const llvm::Value* llvmVal = llvmModuleSet->getLLVMValue(svfFun);
    const llvm::Function* llvmFun = SVFUtil::dyn_cast<llvm::Function>(llvmVal);

    if (!llvmFun) {
        GraphReaderUtil::sendJsonError("Could not retrieve LLVM function for '" + functionName + "'.");
        return;
    }

    result = GraphReaderUtil::getFunctionInfoJson(llvmFun);
    result["error"] = false;
    llvm::outs() << llvm::formatv("{0:2}", llvm::json::Value(std::move(result))) << "\n";
}

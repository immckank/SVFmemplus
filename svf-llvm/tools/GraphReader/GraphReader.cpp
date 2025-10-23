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

// Helper function to convert SVFGNode kind to string.
// This is needed because SVFGNode itself does not have getNodeKindStr().
std::string getSVFGNodeKindString(const SVFGNode* node) {
    if (!node) return "Null";
    switch (node->getNodeKind()) {
        case SVF::SVFValue::Addr: return "Addr";
        case SVF::SVFValue::Copy: return "Copy";
        case SVF::SVFValue::Gep: return "Gep";
        case SVF::SVFValue::Store: return "Store";
        case SVF::SVFValue::Load: return "Load";
        case SVF::SVFValue::Cmp: return "Cmp";
        case SVF::SVFValue::BinaryOp: return "BinaryOp";
        case SVF::SVFValue::UnaryOp: return "UnaryOp";
        case SVF::SVFValue::Branch: return "Branch";
        case SVF::SVFValue::DummyVProp: return "DummyVProp";
        case SVF::SVFValue::NPtr: return "NPtr";
        case SVF::SVFValue::FRet: return "FRet";
        case SVF::SVFValue::ARet: return "ARet";
        case SVF::SVFValue::AParm: return "AParm";
        case SVF::SVFValue::FParm: return "FParm";
        case SVF::SVFValue::TPhi: return "TPhi";
        case SVF::SVFValue::TIntraPhi: return "TIntraPhi";
        case SVF::SVFValue::TInterPhi: return "TInterPhi";
        case SVF::SVFValue::FPIN: return "FPIN";
        case SVF::SVFValue::FPOUT: return "FPOUT";
        case SVF::SVFValue::APIN: return "APIN";
        case SVF::SVFValue::APOUT: return "APOUT";
        case SVF::SVFValue::MPhi: return "MPhi";
        case SVF::SVFValue::MIntraPhi: return "MIntraPhi";
        case SVF::SVFValue::MInterPhi: return "MInterPhi";
        default: return "UnknownVFGNodeKind";
    }
}


/*!
 * \brief 根据源代码位置和可选的操作数索引查找SVFGNode。
 *
 * 这是一个更可靠的映射方法，解决了ICFGNode到SVFGNode一对多的问题。
 * 1. 从 location 找到 ICFGNode，然后获取对应的 llvm::Instruction。
 * 2. 收集指令的定义值（LHS）和所有操作数（RHS）。
 * 3. 将它们都转换为 SVFGNode，并存储在一个列表中。
 * 4. 用户可以通过 operandIndex 选择想要的节点：
 *    - -1 (默认): 返回定义值 (LHS) 对应的节点。
 *    - 0, 1, 2...: 返回第 n 个操作数 (RHS) 对应的节点。
 *
 * \param svfg 指向SVFG的指针。
 * \param icfg 指向ICFG的指针。
 * \param location 形如 "filename:line" 的字符串。
 * \param operandIndex 操作数索引。
 * \return 如果找到，则为匹配的SVFGNode指针，否则为nullptr。
 */
const SVFGNode* findSVFGNodeByLocation(SVFG* svfg, ICFG* icfg, const std::string& location, int operandIndex = -1) {
    std::string opIndexStr = (operandIndex == -1) ? "LHS (defined value)" : std::to_string(operandIndex);
    SVF::SVFUtil::outs() << "Debug: Searching for SVFGNode at location '" << location << "' with operand index " << opIndexStr << "...\n";
    const ICFGNode* icfgNode = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
    if (!icfgNode) {
        SVF::SVFUtil::errs() << "Error: Cannot find ICFGNode for location: " << location << "\n";
        return nullptr;
    }

    SVF::SVFUtil::outs() << "Debug: Found ICFGNode with ID: " << icfgNode->getId() << "\n";

    // Get the LLVM instruction directly from the ICFGNode.
    // This is more robust than relying on SVFStmts, as some instructions (like 'free')
    // have an ICFGNode but no SVFStmt. We handle different ICFGNode types.
    const llvm::Instruction* inst = nullptr;
    if (auto intraNode = SVFUtil::dyn_cast<IntraICFGNode>(icfgNode)) {
        const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(intraNode);
        inst = SVFUtil::dyn_cast<llvm::Instruction>(llvmVal);
    } else if (auto callNode = SVFUtil::dyn_cast<CallICFGNode>(icfgNode)) {
        const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(callNode);
        inst = SVFUtil::dyn_cast<llvm::Instruction>(llvmVal);
    } else if (auto retNode = SVFUtil::dyn_cast<RetICFGNode>(icfgNode)) {
        const llvm::Value* llvmVal = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(retNode);
        inst = SVFUtil::dyn_cast<llvm::Instruction>(llvmVal);
    }
    SVF::SVFUtil::outs() << "Debug: Retrieved LLVM instruction from ICFGNode at " << location << ".\n";
    if (!inst) {
        SVF::SVFUtil::errs() << "Error: Could not retrieve LLVM instruction from ICFGNode at " << location << ".\n";
        if (icfgNode->getSVFStmts().empty()) {
             SVF::SVFUtil::outs() << "Debug: This ICFGNode also has no associated SVF statements.\n";
        }
        return nullptr;
    }

    // The core issue is that PAG/SVFIR node IDs do not map directly to SVFG node IDs.
    // We must find the SVFGNode through the SVFStmts associated with the ICFGNode.
    const llvm::Value* targetLLVMValue = nullptr;
    if (operandIndex == -1) { // User wants the LHS (defined value)
        if (inst->getType()->isVoidTy()) {
            SVF::SVFUtil::errs() << "Warning: Instruction at " << location << " does not define a value (e.g., store, free). Try specifying an operand index like --start-op 0.\n";
            return nullptr;
        }
        targetLLVMValue = inst;
    } else { // User wants an RHS operand
        if (operandIndex < 0 || static_cast<unsigned>(operandIndex) >= inst->getNumOperands()) {
            SVF::SVFUtil::errs() << "Error: Invalid operand index " << opIndexStr << " for instruction at " << location
                                 << ". It has " << inst->getNumOperands() << " operands (0 to " << inst->getNumOperands() - 1 << ").\n";
            return nullptr;
        }
        targetLLVMValue = inst->getOperand(operandIndex);
    }

    if (!targetLLVMValue) {
        SVF::SVFUtil::errs() << "Error: Could not determine target LLVM value for operand " << opIndexStr << " at " << location << ".\n";
        return nullptr;
    }

    // Iterate through all SVFStmts in the ICFGNode to find the one that defines our target value.
    for (const SVFStmt* stmt : icfgNode->getSVFStmts()) {
        const SVFGNode* svfgNode = svfg->getSVFGNode(stmt->getEdgeID()); // Corrected: use getEdgeID()
        if (!svfgNode) continue;
        
        // Directly check the SVFGNode's associated LLVM value
        // SVFGNode has getVal() which returns SVFVar*, and SVFVar has getLLVMValue()
        // Correction: The base VFGNode::getValue() returns nullptr. We must cast to a derived
        // type that actually implements getValue() to return a meaningful SVFVar.
        // StmtVFGNode, PHIVFGNode, and ArgumentVFGNode are major classes that do this.
        const SVFVar* svfVar = nullptr;
        if (const auto* stmtNode = SVFUtil::dyn_cast<StmtVFGNode>(svfgNode)) {
            svfVar = stmtNode->getValue(); // getValue() on StmtVFGNode returns the destination SVFVar
        } else if (const auto* phiNode = SVFUtil::dyn_cast<PHIVFGNode>(svfgNode)) {
            svfVar = phiNode->getValue(); // getValue() on PHIVFGNode returns the result SVFVar
        } else if (const auto* argNode = SVFUtil::dyn_cast<ArgumentVFGNode>(svfgNode)) {
            svfVar = argNode->getValue(); // getValue() on ArgumentVFGNode returns the parameter SVFVar
        }

        if (svfVar) {
            // There is no direct getLLVMValue() on SVFVar.
            // We must get the ICFGNode from the SVFVar, and then get the llvm::Value from it.
            const ICFGNode* varIcfgNode = nullptr;
            if (const auto* valVar = SVFUtil::dyn_cast<ValVar>(svfVar)) {
                varIcfgNode = valVar->getICFGNode();
            } else if (const auto* objVar = SVFUtil::dyn_cast<BaseObjVar>(svfVar)) {
                // BaseObjVar and its children (Stack, Heap, Global) have an ICFGNode
                varIcfgNode = objVar->getICFGNode();
            }

            if (varIcfgNode && LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(varIcfgNode) == targetLLVMValue) {
            SVF::SVFUtil::outs() << "Debug: Found matching SVFGNode for value at " << location << ".\n";
            SVF::SVFUtil::outs() << "       |-- SVFGNode ID: " << svfgNode->getId() << "\n";
            SVF::SVFUtil::outs() << "       |-- SVFGNode Type: " << getSVFGNodeKindString(svfgNode) << "\n";
            SVF::SVFUtil::outs() << "       |-- Statement: " << *svfgNode << "\n";
            return svfgNode;
            }
        }
    }

    SVF::SVFUtil::errs() << "Error: Could not find a matching SVFGNode for operand " << opIndexStr << " at location " << location << ".\n";
    SVF::SVFUtil::errs() << "This might happen if the value is a constant or not a pointer type, and thus not represented in the SVFG.\n";
    return nullptr;
}

/*!
 * \brief Displays all SVFG nodes associated with a given source code location.
 *
 * This function first finds the ICFGNode corresponding to the provided source location.
 * Then, it retrieves all SVF statements associated with that ICFGNode and, for each
 * statement, finds and prints details about the corresponding SVFGNode.
 *
 * \param svfg Pointer to the SVFG.
 * \param icfg Pointer to the ICFG.
 * \param location Source code location string (e.g., "filename:line").
 */
const SVFGNode* showSVFGNodeByLocation(SVFG* svfg, ICFG* icfg, const std::string& location) {
    SVF::SVFUtil::outs() << "Searching for SVFG nodes at location '" << location << "'...\n";
    const ICFGNode* icfgNode = GraphReaderUtil::findICFGNodeByLocation(icfg, location);
    if (!icfgNode) {
        SVF::SVFUtil::errs() << "Error: Cannot find ICFGNode for location: " << location << "\n";
        return nullptr;
    }
    SVF::SVFUtil::outs() << "Found ICFGNode with ID: " << icfgNode->getId() << " at " << location << "\n";

    //使用 SVFG 的迭代器 svfg->begin() 和 svfg->end() 来遍历所有节点。
    for (auto& pair : *svfg) {
        SVFGNode* node = pair.second;
        if (node->getICFGNode() == icfgNode) {
            SVF::SVFUtil::outs() << "Found SVFGNode with ID: " << node->getId() << "\n";
            SVF::SVFUtil::outs() << "SVFGNode info: " << node->toString() << "\n";
            return node;
        }
    }
    return nullptr;
}


/*!
 * GraphReader: A tool to read and analyze SVF graphs.
 * It uses SVF's command-line option system for analysis selection.
 */
int main(int argc, char ** argv) {
    std::vector<std::string> moduleNameVec = 
        OptionBase::parseOptions(argc, argv, "GraphReader", "[options] <input-bitcode...>");

    LLVMModuleSet::buildSVFModule(moduleNameVec);
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();

    AndersenWaveDiff* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    ICFG* icfg = ander->getICFG();

    auto memSSA = std::make_unique<GraphReaderSVFGBuilder>();
    SVFG* svfg = memSSA->buildFullSVFG(ander);

    FunctionQuery fq(icfg, pag);

    if (!Options::FindCallSites().empty()) {
        fq.findCallSites(Options::FindCallSites());
    }
    else if (!Options::FindCalleeBody().empty()) {
        fq.findCalleeBodyByLocation(Options::FindCalleeBody());
    }
    else if (!Options::FindFuncBody().empty()) {
        fq.findFunctionBodyByLocation(Options::FindFuncBody());
    }
    else if (!Options::FindBodyByName().empty()) {
        fq.findFunctionBodyByName(Options::FindBodyByName());
    }
    else if (!Options::FindAllCallees().empty()) {
        fq.findAllCalleesByName(Options::FindAllCallees());
    }
    else if (!Options::FindVarByLocation().empty()) {
        GraphReaderUtil::findDefinedVarByLocation(pag, svfg, Options::FindVarByLocation());
    }
    else if (!Options::PathCondFuncStart().empty() && !Options::PathCondFuncEnd().empty()) {
        // SVFG is not needed for this query, so we can pass nullptr.
        PathQuery pq(nullptr, icfg);
        pq.getConditionPath(Options::PathCondFuncStart(), Options::PathCondFuncEnd());
    }
    else if (!Options::ValuePathStart().empty()) {
        int operandIndex = -1;
        try {
            operandIndex = std::stoi(Options::ValuePathOp());
        } catch (const std::invalid_argument& ia) {
            SVF::SVFUtil::errs() << "Warning: Invalid operand index '" << Options::ValuePathOp() << "'. Using default -1.\n";
        } catch (const std::out_of_range& oor) {
            SVF::SVFUtil::errs() << "Warning: Operand index '" << Options::ValuePathOp() << "' is out of range. Using default -1.\n";
        }
        const SVFGNode* startNode = findSVFGNodeByLocation(svfg, icfg, Options::ValuePathStart(), operandIndex);
        if (startNode) {
            PathQuery pq(svfg, icfg);
            pq.getValuePath(startNode);
        } else {
            GraphReaderUtil::sendJsonError("Could not find start node for value path analysis. Check location and operand index.");
        }
    }
    else if (!Options::ValuePathInsideStart().empty()) {
        int operandIndex = -1;
        try {
            operandIndex = std::stoi(Options::ValuePathOp());
        } catch (const std::invalid_argument& ia) {
            SVF::SVFUtil::errs() << "Warning: Invalid operand index '" << Options::ValuePathOp() << "' for intra-procedural value path. Using default -1.\n";
        } catch (const std::out_of_range& oor) {
            SVF::SVFUtil::errs() << "Warning: Operand index '" << Options::ValuePathOp() << "' for intra-procedural value path is out of range. Using default -1.\n";
        }
        const SVFGNode* startNode = showSVFGNodeByLocation(svfg, icfg, Options::ValuePathInsideStart());
        if (startNode) {
            PathQuery pq(svfg, icfg);
            pq.getValueInsidePath(startNode);
        } else {
            GraphReaderUtil::sendJsonError("Could not find start node for intra-procedural value path analysis. Check location and operand index.");
            SVF::SVFUtil::errs() << operandIndex << "Warning: Operand index '" << Options::ValuePathOp() << "' for intra-procedural value path is out of range. Using default -1.\n";
        }
    }

    else if (!Options::PathCondStart().empty() && !Options::PathCondEnd().empty()) {
        // svfgPathCondReader(svfg, icfg, Options::PathCondStart(), Options::PathCondEnd());
    }
    else if (!Options::PathCondInsideStart().empty() && !Options::PathCondInsideEnd().empty()) {
        PathQuery pq(nullptr, icfg);
        pq.getConditionInsidePath(Options::PathCondInsideStart(), Options::PathCondInsideEnd());
    }
    else if (!Options::ShowSVFGNode().empty()) {
        showSVFGNodeByLocation(svfg, icfg, Options::ShowSVFGNode());
    }
    else {
        SVF::SVFUtil::outs() << "No analysis option specified. Use --help to see available options.\n";
        SVF::SVFUtil::outs() << "Defaulting to an example: finding call sites for 'stats_prefix_record_get'\n";
        fq.findCallSites("stats_prefix_record_get");
    }
    return 0;
}

// graph-reader -find-var-by-location slabs:162 PUT/memcached.bc
// graph-reader -find-var-by-location slabs:163 PUT/memcached.bc
// graph-reader -find-var-by-location slabs:164 PUT/memcached.bc
// graph-reader -find-var-by-location items.c:1557 PUT/memcached.bc
// graph-reader -path-cond-func-start items.c:1557 -path-cond-func-end items.c:1630 PUT/memcached.bc
// graph-reader -find-function-body items.c:1557 PUT/memcached.bc

// graph-reader -find-body-by-name TIFFWriteDirectoryTagCheckedLongArray PUT/libtiff-57449991.bc
// graph-reader -find-function-body tif_dirwrite.c:1893 PUT/libtiff-57449991.bc
// graph-reader -find-var-by-location tif_dirread.c:231 PUT/libtiff-57449991.bc 236

// graph-reader -value-path-start=tif_dirwrite.c:1865 -value-path-op=0 PUT/libtiff-57449991.bc

// graph-reader -path-cond-inside-start=tif_dirwrite.c:1360 -path-cond-inside-end=tif_dirwrite.c:1369 PUT/libtiff-57449991.bc
// graph-reader -value-path-inside-start=tif_dirwrite.c:1893 -value-path-op=-1 PUT/libtiff-57449991.bc

// graph-reader -show-svfg-node=tif_dirwrite.c:1893 PUT/libtiff-57449991.bc
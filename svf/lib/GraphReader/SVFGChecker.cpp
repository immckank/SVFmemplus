// #include "GraphReader/SVFGChecker.h"
// #include "Graphs/SVFGStat.h"
// #include "Graphs/ICFG.h"
// #include "SVFIR/SVFIR.h"
// #include "SVFIR/SVFValue.h"

// using namespace SVF;
// using namespace SVFUtil;

// /// 重载 initSrcs，使用我们自定义的逻辑
// void SVFGChecker::initSrcs() {
//     SVFGNodeSet nodes = findSVFGNodesByLocationAndVar(sourceLocs);
//     for (const SVFGNode* node : nodes) {
//         addToSources(node);
//     }
// }

// /// 重载 initSnks，使用我们自定义的逻辑
// void SVFGChecker::initSnks() {
//     SVFGNodeSet nodes = findSVFGNodesByLocationAndVar(sinkLocs);
//     for (const SVFGNode* node : nodes) {
//         addToSinks(node);
//     }
// }

// /// 重载 reportBug，打印我们想要的信息
// void SVFGChecker::reportBug(ProgSlice* slice) {
//     const SVFGNode* source = slice->getSource();
//     const SVFGNodeSet& reachableSinks = slice->getSinks();

//     outs() << "============================================================\n";
//     outs() << "Analysis for Source: " << source->toString() << "\n";
//     outs() << "------------------------------------------------------------\n";

//     if (reachableSinks.empty()) {
//         outs() << "  -> No sinks are reachable from this source.\n";
//     } else {
//         for (const SVFGNode* sink : reachableSinks) {
//             // 获取从 source 到这个 sink 的路径条件
//             ProgSlice::Condition cond = slice->getVFCond(sink);
//             // 检查条件是否可满足 (即是否存在真实路径)
//             if (!slice->isEquivalentBranchCond(cond, slice->getFalseCond())) {
//                 outs() << "  -> Reachable Sink: " << sink->toString() << "\n";
//                 outs() << "     Path Condition:\n";
//                 std::string condStr = slice->evalFinalCond();
//                 if (condStr.empty() || condStr.find_first_not_of(" \t\n\v\f\r") == std::string::npos) { // Check if string is empty or just whitespace
//                     outs() << "\t\t  --> (Unconditional Path)\n";
//                 } else {
//                     outs() << condStr;
//                 }
//             }
//         }
//     }
//     outs() << "============================================================\n\n";
// }

// /// 辅助函数实现：根据位置和变量名查找节点
// SVFGChecker::SVFGNodeSet SVFGChecker::findSVFGNodesByLocationAndVar(const std::vector<LocationAndVar>& locs) {
//     SVFIR* pag = getPAG();
//     const SVFG* svfg = getSVFG();
//     SVFGNodeSet nodeSet;

//     for (const auto& locVar : locs) {
//         size_t colon_pos = location.find(':');
//         if (colon_pos == std::string::npos) {
//             continue;
//         }
//         std::string filename = location.substr(0, colon_pos);
//         std::string line_str = location.substr(colon_pos + 1);

//         // 遍历PAG中的所有语句来查找匹配的指令
//         for (const SVFStmt* stmt : pag->getSVFStmtSet()) {
//             const llvm::Instruction* inst = stmt->getInst();
//             if (!inst) continue;

//             const llvm::DebugLoc& dloc = inst->getDebugLoc();
//             if (dloc && dloc.getLine() == line && dloc.getCol() == col && dloc.getFilename().endswith(filename)) {
//                 // 找到了匹配位置的指令，现在检查是否为我们关心的变量赋值
//                 if (const AssignPE* assignStmt = SVFUtil::dyn_cast<AssignPE>(stmt)) {
//                     if (const PAGNode* pagNode = assignStmt->getDef()) {
//                         const SVFValue* svfVal = pag->getSVFValue(pagNode);
//                         if (svfVal && svfVal->hasName() && svfVal->getName() == locVar.varName) {
//                         if (const SVFGNode* svfgNode = svfg->getDefSVFGNode(pagNode)) {
//                             nodeSet.insert(svfgNode);
//                             outs() << "Found Source/Sink Node " << svfgNode->getId() << " for " << locVar.location << " var " << locVar.varName << "\n";
//                         }
//                     }
//                     }
//                 }
//                 // 也可以检查源操作数
//                 // for (auto op = stmt->op_begin(); op != stmt->op_end(); ++op) { ... }
//             }
//         }
//     }
//     return nodeSet;
// }

// /// 辅助函数：解析 "file:line:col" 格式
// bool SVFGChecker::parseLocation(const std::string& location, std::string& filename, unsigned& line, unsigned& col) {
//     size_t first_colon = location.find(':');
//     if (first_colon == std::string::npos) {
//         return false;
//     }
//     size_t second_colon = location.find(':', first_colon + 1);
//     if (second_colon == std::string::npos) {
//         return false;
//     }

//     filename = location.substr(0, first_colon);
//     try {
//         line = std::stoul(location.substr(first_colon + 1, second_colon - first_colon - 1));
//         col = std::stoul(location.substr(second_colon + 1));
//     } catch (const std::invalid_argument& ia) {
//         return false;
//     }
//     return true;
// }
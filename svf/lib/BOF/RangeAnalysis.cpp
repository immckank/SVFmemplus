//===----- RangeAnalysis.cpp -- Range Analysis of SVF Vars --------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

/*
 *  RangeAnalysis.cpp 
 *
 *  Created on: Mar 15 , 2025
 *      Author: Yaokun Yang
 */

#include "BOF/RangeAnalysis.h"
using namespace SVF;
using namespace std;

void RangeAnalysis::analysisBufferRange(const StackObjVar* stackObjVar){
    u64_t size = 0;
    // handle case: %arrayidx = alloca [10*i32], align 16
    const SVFType* objType = stackObjVar->getType();
    const StInfo* objInfo = objType->getTypeInfo();
    size = objInfo->getNumOfFlattenElements();
    if(size > 1)
    {
        Range buffer_size = Range(0, size);
        bufferRanges[stackObjVar] = buffer_size;
        return;
    }


    // handle case: int a[n] -> %a = alloca i32, i32 %n, align 4
    size = stackObjVar->getNumOfElements();
    if(size > 1)
    {      
        Range buffer_size  = Range(0, size);
        bufferRanges[stackObjVar] = buffer_size;
        return;
    }

    // handle case: int a[n] -> %a = alloca i32, i32 %n, align 4
    // size = 0;
    // const std::vector<SVFVar*>& sizeVec = addrStmt->getArrSize();
    // if(sizeVec.size() > 0)
    // {   
    //     if(const auto constVar = SVFUtil::dyn_cast<ConstIntObjVar>(sizeVec[0]))
    //         size = constVar->getZExtValue();
    //     cout << "size from vec = " << sizeVec.size() << endl;
    //     if(size > 1){
    //         cout << "add case from type, size = " << size << endl;
    //         this->worklist.push(BFSNode(dest, 0, size));
    //         // continue;
    //     }
            
    // }
    bufferRanges[stackObjVar] = Range::BOTTOM;
};
         


Range RangeAnalysis::analysisIndexRange(const SVFVar* index, const GepStmt* gepStmt){
    // calculate offset
    AccessPath ap = gepStmt->getAccessPath();
    s64_t offset = ap.computeConstantOffset();
    return Range(offset, offset);
};

// // TODO
// Range analyze(const SVFVar* var) {
//     // 1. 用 map 作为 visited + memo
//     auto it = indexRanges.find(var);
//     if (it != indexRanges.end()) {
//         return it->second;
//     }

//     // 先放一个 UNDEFINED 防止递归环
//     indexRanges[var] = Range::UNDEFINED;

//     // 2. 拿到 LLVM Value
//     auto llvmVal =
//         LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(var);

//     if (!llvmVal) {
//         return Range::UNDEFINED;
//     }

//     Range result = Range::UNDEFINED;

//     // -------------------------
//     // 3. 常量
//     // -------------------------
//     if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(llvmVal)) {
//         long long v = ci->getSExtValue();
//         result = Range(v, v);
//     }

//     // -------------------------
//     // 4. BinaryOperator
//     // -------------------------
//     else if (auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(llvmVal)) {

//         auto* lhsVal = bin->getOperand(0);
//         auto* rhsVal = bin->getOperand(1);

//         const SVFVar* lhs = LLVMModuleSet::getLLVMModuleSet()
//                                 ->getSVFVar(lhsVal);
//         const SVFVar* rhs = LLVMModuleSet::getLLVMModuleSet()
//                                 ->getSVFVar(rhsVal);

//         Range r1 = analyze(lhs);
//         Range r2 = analyze(rhs);

//         if (!r1.isUndefined() && !r2.isUndefined()) {

//             switch (bin->getOpcode()) {

//                 case llvm::Instruction::Add:
//                     result = r1 + r2;
//                     break;

//                 case llvm::Instruction::Sub:
//                     result = Range(
//                         r1.getLower() - r2.getUpper(),
//                         r1.getUpper() - r2.getLower()
//                     );
//                     break;

//                 case llvm::Instruction::Mul:
//                     result = multiplyRange(r1, r2);
//                     break;

//                 default:
//                     result = Range::UNDEFINED;
//             }
//         }
//     }

//     // -------------------------
//     // 5. PHI（关键）
//     // -------------------------
//     else if (auto* phi = llvm::dyn_cast<llvm::PHINode>(llvmVal)) {

//         for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {

//             auto* incomingVal = phi->getIncomingValue(i);

//             const SVFVar* v =
//                 LLVMModuleSet::getLLVMModuleSet()->getSVFVar(incomingVal);

//             Range r = analyze(v);

//             result = Range::merge(result, r);
//         }
//     }

//     // -------------------------
//     // 6. Cast（常见）
//     // -------------------------
//     else if (auto* castInst = llvm::dyn_cast<llvm::CastInst>(llvmVal)) {

//         auto* src = castInst->getOperand(0);
//         const SVFVar* v =
//             LLVMModuleSet::getLLVMModuleSet()->getSVFVar(src);

//         result = analyze(v);
//     }

//     // -------------------------
//     // 7. 其他（load/call/GEP等）
//     // -------------------------
//     else {
//         result = Range::UNDEFINED;
//     }

//     // 8. 写回缓存（关键）
//     indexRanges[var] = result;

//     return result;
// }



Range RangeAnalysis::getBufferRange(const SVFVar* buffer){
    auto it = bufferRanges.find(buffer);
    if (it != bufferRanges.end()) {
        return it->second;
    }
    return Range::BOTTOM;
};


            
Range RangeAnalysis::getIndexRange(const SVFVar* index){
    auto it = indexRanges.find(index);
    if (it != indexRanges.end()) {
        return it->second;
    }
    return Range::BOTTOM;
};
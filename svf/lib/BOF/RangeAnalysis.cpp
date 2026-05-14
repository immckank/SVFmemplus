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

const int RangeAnalysis::MAX_RECURSION_DEPTH = 200;

RangeAnalysis::RangeAnalysis(){
    
}

bool RangeAnalysis::analyzeBufferRange(const StackObjVar* stackObjVar){
    u64_t size = 0;
    // int a[16] -> %arrayidx = alloca [10*i32], align 16
    const SVFType* objType = stackObjVar->getType();
    const StInfo* objInfo = objType->getTypeInfo();
    size = objInfo->getNumOfFlattenElements();
    if(size > 1)
    {
        Range buffer_size = Range(0, size-1);
        bufferRanges[stackObjVar] = buffer_size;
        return true;
    }


    // int a[n] -> %a = alloca i32, i32 %n, align 4
    size = stackObjVar->getNumOfElements();
    if(size > 1)
    {      
        Range buffer_size  = Range(0, size-1);
        bufferRanges[stackObjVar] = buffer_size;
        return true;
    }
    return false;
};

bool RangeAnalysis::analyzeBufferRange(const HeapObjVar* heapObjVar){
    if(!heapObjVar->isConstantByteSize())
        return false;

    u64_t size = heapObjVar->getByteSizeOfObj();
    return setBufferByteRange(heapObjVar, Range(static_cast<Range::BoundType>(size)));
}

bool RangeAnalysis::setBufferByteRange(const SVFVar* buffer, const Range& byteSizeRange){
    if(byteSizeRange.isBottom() || byteSizeRange.isTop())
        return false;

    Range::BoundType upper = byteSizeRange.getUpper();
    if(upper == Range::INF || upper <= 0)
        return false;

    bufferRanges[buffer] = Range(0, upper - 1);
    return true;
}
         

Range RangeAnalysis::analyzeVarRange(const SVFVar* var, int depth) {
    // ==== Check if the maximum recursion depth is reached ====
    if(depth == MAX_RECURSION_DEPTH){
        return Range::TOP;
    }

    // ===== Attempt to retrieve range from cache =====
    Range var_range_find = getVarRange(var);
    // Cache hit: Found a valid range (including TOP).
    if(!var_range_find.isBottom())
        return var_range_find;
        
    /// ===== Constant Value Calculation =====
    if(const ConstIntValVar* intVar = SVFUtil::dyn_cast<ConstIntValVar>(var)){
        varRanges[var] = Range(intVar->getSExtValue());
        return varRanges[var];
    }

    // ===== Dynamic Value Calculation =====
    // Handling dynamic instructions that affect the range:
    // This includes operations such as: 
    // - Address calculation (Addr)
    // - Variable copy (Copy)
    // - Memory operations (Store, Load)
    // - Instruction branches (Gep, Phi, Select, Cmp, BinaryOp, UnaryOp, Branch)
    // Instructions are not supported:
    // - Function calls (Call, Ret)
    // - Thread-related operations (ThreadFork, ThreadJoin)
    varRanges[var] = Range::TOP;
    Range var_range = Range::BOTTOM;
    SVFStmt::PEDGEK checkType;

    checkType = SVFStmt::PEDGEK::Load;
    if(var->hasIncomingEdges(checkType)){
        // OrderedSet<SVFStmt*>
        for(auto stmt = var->getIncomingEdgesBegin(checkType); stmt != var->getIncomingEdgesEnd(checkType); ++stmt){
            if(const LoadStmt* loadStmt = SVFUtil::dyn_cast<LoadStmt>(*stmt)){
                const SVFVar* rhs = loadStmt->getRHSVar(); 
                var_range = Range::join(var_range, analyzeVarRange(rhs, depth+1));
            }
        }
    }

    checkType = SVFStmt::PEDGEK::Store;
    if(var->hasIncomingEdges(checkType)){
        // OrderedSet<SVFStmt*>
        for(auto stmt = var->getIncomingEdgesBegin(checkType); stmt != var->getIncomingEdgesEnd(checkType); ++stmt){
            if(const StoreStmt* storeStmt = SVFUtil::dyn_cast<StoreStmt>(*stmt)){
                const SVFVar* rhs = storeStmt->getRHSVar(); 
                var_range = Range::join(var_range, analyzeVarRange(rhs, depth+1));
            }
        }
    }

    checkType = SVFStmt::PEDGEK::Copy;
    if(var->hasIncomingEdges(checkType)){
        // OrderedSet<SVFStmt*>
        for(auto stmt = var->getIncomingEdgesBegin(checkType); stmt != var->getIncomingEdgesEnd(checkType); ++stmt){
            if(const CopyStmt* copyStmt = SVFUtil::dyn_cast<CopyStmt>(*stmt)){
                const SVFVar* rhs = copyStmt->getRHSVar(); 
                var_range = Range::join(var_range, analyzeVarRange(rhs, depth+1)); // TODO: handle other type of copy
            }
        }
    }
    
    checkType = SVFStmt::PEDGEK::BinaryOp;
    if(var->hasIncomingEdges(checkType)){
        // OrderedSet<SVFStmt*>
        for(auto stmt = var->getIncomingEdgesBegin(checkType); stmt != var->getIncomingEdgesEnd(checkType); ++stmt){
            if(const BinaryOPStmt* binaryOPStmt = SVFUtil::dyn_cast<BinaryOPStmt>(*stmt)){
                if(binaryOPStmt->getOpVarNum() == 2){
                    const SVFVar* op1 = binaryOPStmt->getOpVar(0);
                    const SVFVar* op2 = binaryOPStmt->getOpVar(1);
                    
                    switch(binaryOPStmt->getOpcode()){
                        // FAdd = 14,      // Sum of floats
                        // FSub = 16,      // Subtraction of floats
                        // FMul = 18,      // Product of floats.
                        // FDiv = 21,      // Float division.
                        // FRem = 24,      // Float remainder
                        case BinaryOPStmt::OpCode::Add:  // Sum of integers
                            var_range = Range::join(var_range, Range::add(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::Sub:  // Subtraction of integers
                            var_range = Range::join(var_range, Range::sub(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::Mul:  // Product of integers
                            var_range = Range::join(var_range, Range::mul(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::UDiv:  // Unsigned division
                            var_range = Range::join(var_range, Range::div(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::SDiv:  // Signed division
                            var_range = Range::join(var_range, Range::div(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::URem:  // Unsigned remainder
                            var_range = Range::join(var_range, Range::mod(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::SRem:  // Signed remainder
                            var_range = Range::join(var_range, Range::mod(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::Shl:  // Shift left  (logical)
                            var_range = Range::join(var_range, Range::shl(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::LShr:  // Shift right (logical)
                            var_range = Range::join(var_range, Range::lshr(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                        // TODO: change ashr of Range
                        case BinaryOPStmt::OpCode::AShr:  // Shift right (arithmetic)
                            var_range = Range::join(var_range, Range::ashr(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::And:  // Logical and
                            var_range = Range::join(var_range, Range::bit_and(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::Or:  // Logical or
                            var_range = Range::join(var_range, Range::bit_or(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::Xor:  // Logical xor
                            var_range = Range::join(var_range, Range::bit_xor(analyzeVarRange(op1, depth+1), analyzeVarRange(op2, depth+1)));
                            break;
                    }
                }
            }
        }
    }
    
    // Put in cache. Values without a supported defining edge are unknown, not
    // unreachable; propagate TOP so callers still perform conservative checks.
    if(!var_range.isBottom())
        varRanges[var] = var_range;
    return varRanges[var];
}



Range RangeAnalysis::getBufferRange(const SVFVar* buffer){
    auto it = bufferRanges.find(buffer);
    if (it != bufferRanges.end()) {
        return it->second;
    }
    return Range::BOTTOM;
};


            
Range RangeAnalysis::getVarRange(const SVFVar* var){
    auto it = varRanges.find(var);
    if (it != varRanges.end()) {
        return it->second;
    }
    return Range::BOTTOM;
};

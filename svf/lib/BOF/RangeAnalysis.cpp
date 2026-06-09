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
#include "SVFIR/SVFIR.h"
using namespace SVF;
using namespace std;

const int RangeAnalysis::MAX_RECURSION_DEPTH = 200;
const int RangeAnalysis::MAX_AFFINE_DEPTH = 60;
const size_t RangeAnalysis::MAX_AFFINE_TERMS = 16;

RangeAnalysis::RangeAnalysis(){
    
}

bool RangeAnalysis::analyzeBufferRange(const StackObjVar* stackObjVar){
    u64_t size = 0;
    // int a[16] -> %arrayidx = alloca [10*i32], align 16
    const SVFType* objType = stackObjVar->getType();
    const StInfo* objInfo = objType->getTypeInfo();
    size = objInfo->getNumOfFlattenElements();
    if(size >= 1)
    {
        Range buffer_size = Range(0, size-1);
        bufferRanges[stackObjVar] = buffer_size;
        return true;
    }


    // int a[n] -> %a = alloca i32, i32 %n, align 4
    size = stackObjVar->getNumOfElements();
    if(size >= 1)
    {      
        Range buffer_size  = Range(0, size-1);
        bufferRanges[stackObjVar] = buffer_size;
        return true;
    }
    return false;
};

bool RangeAnalysis::analyzeBufferRange(const HeapObjVar* heapObjVar){
    // getByteSizeOfObj() asserts on non-constant-size objects, so guard it.
    if(!heapObjVar->isConstantByteSize())
        return false;
    u64_t size = heapObjVar->getByteSizeOfObj();
    if(size >= 1)
    {      
        Range buffer_size  = Range(0, size-1);
        bufferRanges[heapObjVar] = buffer_size;
        return true;
    }
    return false;
}
         

Range RangeAnalysis::analyzeVarRange(const SVFVar* var, int depth) {
    return analyzeVarRange(var, (const ICFGNode*)nullptr, depth);
}

Range RangeAnalysis::analyzeVarRange(const SVFVar* var, const ICFGNode* context, int depth) {
    // ==== Check if the maximum recursion depth is reached ====
    if(depth == MAX_RECURSION_DEPTH){
        return Range::TOP;
    }

    // ===== Context-sensitive formal binding (k=1 call site) =====
    // If this variable is a callee formal parameter bound at the current call
    // site, return the actual-argument range directly instead of crossing the
    // Call edge (which would otherwise degrade to TOP).
    if(context){
        auto fb = formalBindings.find(std::make_pair(context, var));
        if(fb != formalBindings.end())
            return fb->second;
    }

    // ===== Attempt to retrieve range from cache =====
    Range var_range_find = getCachedVarRange(context, var);
    // Cache hit: Found a valid range (not Bottom)
    if(!var_range_find.isBottom())
        return var_range_find;
        
    /// ===== Constant Value Calculation =====
    if(const ConstIntValVar* intVar = SVFUtil::dyn_cast<ConstIntValVar>(var)){
        Range cst = Range(intVar->getSExtValue());
        setCachedVarRange(context, var, cst);
        return cst;
    }

    // ===== Dynamic Value Calculation =====
    // Handling dynamic instructions that affect the range:
    // This includes operations such as: 
    // - Address calculation (Addr)
    // - Variable copy (Copy)
    // - Memory operations (Store, Load)
    // - Instruction branches (Gep, Phi, Select, Cmp, BinaryOp, UnaryOp, Branch)
    // Cross-function Call/Ret edges are not followed directly here; instead the
    // context-sensitive formal binding above injects the precise actual range.
    // - Thread-related operations (ThreadFork, ThreadJoin)
    setCachedVarRange(context, var, Range::TOP);
    Range var_range = Range::BOTTOM;
    SVFStmt::PEDGEK checkType;

    checkType = SVFStmt::PEDGEK::Load;
    if(var->hasIncomingEdges(checkType)){
        // OrderedSet<SVFStmt*>
        for(auto stmt = var->getIncomingEdgesBegin(checkType); stmt != var->getIncomingEdgesEnd(checkType); ++stmt){
            if(const LoadStmt* loadStmt = SVFUtil::dyn_cast<LoadStmt>(*stmt)){
                const SVFVar* rhs = loadStmt->getRHSVar(); 
                var_range = Range::join(var_range, analyzeVarRange(rhs, context, depth+1));
            }
        }
    }

    checkType = SVFStmt::PEDGEK::Store;
    if(var->hasIncomingEdges(checkType)){
        // OrderedSet<SVFStmt*>
        for(auto stmt = var->getIncomingEdgesBegin(checkType); stmt != var->getIncomingEdgesEnd(checkType); ++stmt){
            if(const StoreStmt* storeStmt = SVFUtil::dyn_cast<StoreStmt>(*stmt)){
                const SVFVar* rhs = storeStmt->getRHSVar(); 
                var_range = Range::join(var_range, analyzeVarRange(rhs, context, depth+1));
            }
        }
    }

    checkType = SVFStmt::PEDGEK::Copy;
    if(var->hasIncomingEdges(checkType)){
        // OrderedSet<SVFStmt*>
        for(auto stmt = var->getIncomingEdgesBegin(checkType); stmt != var->getIncomingEdgesEnd(checkType); ++stmt){
            if(const CopyStmt* copyStmt = SVFUtil::dyn_cast<CopyStmt>(*stmt)){
                const SVFVar* rhs = copyStmt->getRHSVar(); 
                var_range = Range::join(var_range, analyzeVarRange(rhs, context, depth+1)); // TODO: handle other type of copy
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
                            var_range = Range::join(var_range, Range::add(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::Sub:  // Subtraction of integers
                            var_range = Range::join(var_range, Range::sub(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::Mul:  // Product of integers
                            var_range = Range::join(var_range, Range::mul(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::UDiv:  // Unsigned division
                            var_range = Range::join(var_range, Range::div(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::SDiv:  // Signed division
                            var_range = Range::join(var_range, Range::div(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::URem:  // Unsigned remainder
                            var_range = Range::join(var_range, Range::mod(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::SRem:  // Signed remainder
                            var_range = Range::join(var_range, Range::mod(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::Shl:  // Shift left  (logical)
                            var_range = Range::join(var_range, Range::shl(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::LShr:  // Shift right (logical)
                            var_range = Range::join(var_range, Range::lshr(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                        // TODO: change ashr of Range
                        case BinaryOPStmt::OpCode::AShr:  // Shift right (arithmetic)
                            var_range = Range::join(var_range, Range::ashr(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::And:  // Logical and
                            var_range = Range::join(var_range, Range::bit_and(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::Or:  // Logical or
                            var_range = Range::join(var_range, Range::bit_or(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                        case BinaryOPStmt::OpCode::Xor:  // Logical xor
                            var_range = Range::join(var_range, Range::bit_xor(analyzeVarRange(op1, context, depth+1), analyzeVarRange(op2, context, depth+1)));
                            break;
                    }
                }
            }
        }
    }

    // ===== Phi nodes (e.g. loop induction variables, merged control flow) =====
    // var = phi(op0, op1, ...) -> range is the join of all incoming operands.
    // Loop-carried operands recurse back to this var (already marked TOP above),
    // so the result stays a sound over-approximation rather than degrading
    // silently; the must/may classification in the checker handles the [.,TOP]
    // case to avoid false-positive flooding.
    checkType = SVFStmt::PEDGEK::Phi;
    if(var->hasIncomingEdges(checkType)){
        for(auto stmt = var->getIncomingEdgesBegin(checkType); stmt != var->getIncomingEdgesEnd(checkType); ++stmt){
            if(const PhiStmt* phiStmt = SVFUtil::dyn_cast<PhiStmt>(*stmt)){
                for(u32_t i = 0; i < phiStmt->getOpVarNum(); ++i){
                    const SVFVar* opnd = phiStmt->getOpVar(i);
                    var_range = Range::join(var_range, analyzeVarRange(opnd, context, depth+1));
                }
            }
        }
    }

    // ===== Select (cond ? trueValue : falseValue) =====
    checkType = SVFStmt::PEDGEK::Select;
    if(var->hasIncomingEdges(checkType)){
        for(auto stmt = var->getIncomingEdgesBegin(checkType); stmt != var->getIncomingEdgesEnd(checkType); ++stmt){
            if(const SelectStmt* selectStmt = SVFUtil::dyn_cast<SelectStmt>(*stmt)){
                Range condRange = analyzeVarRange(selectStmt->getCondition(), context, depth+1);
                Range trueRange = analyzeVarRange(selectStmt->getTrueValue(), context, depth+1);
                Range falseRange = analyzeVarRange(selectStmt->getFalseValue(), context, depth+1);
                var_range = Range::join(var_range, Range::select(condRange, trueRange, falseRange));
            }
        }
    }
    
    // Put in cache. Values without a supported defining edge are unknown, not
    // unreachable; propagate TOP so callers still perform conservative checks.
    if(!var_range.isBottom())
        setCachedVarRange(context, var, var_range);
    return var_range;
}



Range RangeAnalysis::getBufferRange(const SVFVar* buffer){
    auto it = bufferRanges.find(buffer);
    if (it != bufferRanges.end()) {
        return it->second;
    }
    return Range::BOTTOM;
};

void RangeAnalysis::setBufferRange(const SVFVar* buffer, const Range& range){
    bufferRanges[buffer] = range;
}


void RangeAnalysis::bindFormalRange(const ICFGNode* context, const SVFVar* formal,
                                    const Range& range){
    if(!context || !formal)
        return;
    formalBindings[std::make_pair(context, formal)] = range;
}

Range RangeAnalysis::getCachedVarRange(const ICFGNode* context, const SVFVar* var){
    if(!context)
        return getVarRange(var);
    auto it = ctxVarRanges.find(std::make_pair(context, var));
    if(it != ctxVarRanges.end())
        return it->second;
    return Range::BOTTOM;
}

void RangeAnalysis::setCachedVarRange(const ICFGNode* context, const SVFVar* var,
                                      const Range& range){
    if(!context)
        varRanges[var] = range;
    else
        ctxVarRanges[std::make_pair(context, var)] = range;
}


            
Range RangeAnalysis::getVarRange(const SVFVar* var){
    auto it = varRanges.find(var);
    if (it != varRanges.end()) {
        return it->second;
    }
    return Range::BOTTOM;
};

// ===========================================================================
// Symbolic affine analysis (BOF: prove "allocated < copied" under TOP ranges)
// ===========================================================================
namespace {
/// Merge @p src terms into @p dst, deduplicating and capping the set size.
void mergeAffine(std::vector<SVF::AffineTerm>& dst,
                 const std::vector<SVF::AffineTerm>& src, size_t cap)
{
    for (const auto& t : src)
    {
        bool dup = false;
        for (const auto& e : dst)
            if (e.base == t.base && e.offset == t.offset) { dup = true; break; }
        if (!dup)
        {
            dst.push_back(t);
            if (dst.size() >= cap)
                return;
        }
    }
}
} // namespace

std::string RangeAnalysis::structToken(const SVFVar* var, int depth,
                                       std::set<const SVFVar*>& visited)
{
    if (!var)
        return "?";
    if (depth >= MAX_AFFINE_DEPTH || visited.count(var))
        return "O" + std::to_string(var->getId());

    if (const ConstIntValVar* c = SVFUtil::dyn_cast<ConstIntValVar>(var))
        return "C" + std::to_string(c->getSExtValue());

    visited.insert(var);
    std::string tok;

    // A value forwarded by a copy keeps the same identity.
    if (var->hasIncomingEdges(SVFStmt::PEDGEK::Copy))
    {
        for (auto it = var->getIncomingEdgesBegin(SVFStmt::PEDGEK::Copy);
             it != var->getIncomingEdgesEnd(SVFStmt::PEDGEK::Copy); ++it)
            if (const CopyStmt* cp = SVFUtil::dyn_cast<CopyStmt>(*it))
            {
                tok = structToken(cp->getRHSVar(), depth + 1, visited);
                break;
            }
    }
    // An address produced by a getelementptr: identity = base + index path.
    else if (var->hasIncomingEdges(SVFStmt::PEDGEK::Gep))
    {
        for (auto it = var->getIncomingEdgesBegin(SVFStmt::PEDGEK::Gep);
             it != var->getIncomingEdgesEnd(SVFStmt::PEDGEK::Gep); ++it)
            if (const GepStmt* gep = SVFUtil::dyn_cast<GepStmt>(*it))
            {
                std::string idx;
                const AccessPath& ap = gep->getAccessPath();
                for (const auto& pr : ap.getIdxOperandPairVec())
                {
                    if (const ConstIntValVar* ci =
                            SVFUtil::dyn_cast<ConstIntValVar>(pr.first))
                        idx += std::to_string(ci->getSExtValue()) + ",";
                    else
                        idx += "v" + std::to_string(pr.first->getId()) + ",";
                }
                tok = "G(" + structToken(gep->getRHSVar(), depth + 1, visited) +
                      ";" + idx + ")";
                break;
            }
    }
    // A value loaded from an address: identity = "L(<address identity>)".
    else if (var->hasIncomingEdges(SVFStmt::PEDGEK::Load))
    {
        for (auto it = var->getIncomingEdgesBegin(SVFStmt::PEDGEK::Load);
             it != var->getIncomingEdgesEnd(SVFStmt::PEDGEK::Load); ++it)
            if (const LoadStmt* ld = SVFUtil::dyn_cast<LoadStmt>(*it))
            {
                tok = "L(" + structToken(ld->getRHSVar(), depth + 1, visited) + ")";
                break;
            }
    }

    if (tok.empty())
        tok = "O" + std::to_string(var->getId());

    visited.erase(var);
    return tok;
}

std::string RangeAnalysis::locationToken(const SVFVar* var)
{
    std::set<const SVFVar*> visited;
    return structToken(var, 0, visited);
}

std::vector<AffineTerm> RangeAnalysis::affineStoredInto(const SVFVar* addr,
                                                        std::set<const SVFVar*>& visited,
                                                        int depth)
{
    std::vector<AffineTerm> res;
    if (!addr || !addr->hasIncomingEdges(SVFStmt::PEDGEK::Store))
        return res;
    for (auto it = addr->getIncomingEdgesBegin(SVFStmt::PEDGEK::Store);
         it != addr->getIncomingEdgesEnd(SVFStmt::PEDGEK::Store); ++it)
        if (const StoreStmt* st = SVFUtil::dyn_cast<StoreStmt>(*it))
            mergeAffine(res, analyzeAffine(st->getRHSVar(), visited, depth + 1),
                        MAX_AFFINE_TERMS);
    return res;
}

std::vector<AffineTerm> RangeAnalysis::analyzeAffine(const SVFVar* var)
{
    std::set<const SVFVar*> visited;
    return analyzeAffine(var, visited, 0);
}

std::vector<AffineTerm> RangeAnalysis::analyzeAffine(const SVFVar* var,
                                                     std::set<const SVFVar*>& visited,
                                                     int depth)
{
    if (!var)
        return {};
    // Cycle / depth guard: treat as an opaque symbol identified by the var.
    if (depth >= MAX_AFFINE_DEPTH || visited.count(var))
        return {{"O" + std::to_string(var->getId()), 0}};

    if (const ConstIntValVar* c = SVFUtil::dyn_cast<ConstIntValVar>(var))
        return {{std::string(), (long long)c->getSExtValue()}};

    visited.insert(var);
    std::vector<AffineTerm> res;

    // Copy: same symbolic value.
    if (var->hasIncomingEdges(SVFStmt::PEDGEK::Copy))
        for (auto it = var->getIncomingEdgesBegin(SVFStmt::PEDGEK::Copy);
             it != var->getIncomingEdgesEnd(SVFStmt::PEDGEK::Copy); ++it)
            if (const CopyStmt* cp = SVFUtil::dyn_cast<CopyStmt>(*it))
                mergeAffine(res, analyzeAffine(cp->getRHSVar(), visited, depth + 1),
                            MAX_AFFINE_TERMS);

    // Load: value held at an address. Follow stores into that address; if none
    // (read-only memory), bottom out as an opaque symbol identified by the
    // structural token of the address.
    if (var->hasIncomingEdges(SVFStmt::PEDGEK::Load))
        for (auto it = var->getIncomingEdgesBegin(SVFStmt::PEDGEK::Load);
             it != var->getIncomingEdgesEnd(SVFStmt::PEDGEK::Load); ++it)
            if (const LoadStmt* ld = SVFUtil::dyn_cast<LoadStmt>(*it))
            {
                const SVFVar* addr = ld->getRHSVar();
                std::vector<AffineTerm> stored =
                    affineStoredInto(addr, visited, depth);
                if (!stored.empty())
                    mergeAffine(res, stored, MAX_AFFINE_TERMS);
                else
                {
                    std::string addrTok = structToken(addr, depth + 1, visited);
                    mergeAffine(res, {{"M(" + addrTok + ")", 0}}, MAX_AFFINE_TERMS);
                }
            }

    // BinaryOp Add/Sub with one constant operand: shift the affine offset.
    if (var->hasIncomingEdges(SVFStmt::PEDGEK::BinaryOp))
        for (auto it = var->getIncomingEdgesBegin(SVFStmt::PEDGEK::BinaryOp);
             it != var->getIncomingEdgesEnd(SVFStmt::PEDGEK::BinaryOp); ++it)
        {
            const BinaryOPStmt* bop = SVFUtil::dyn_cast<BinaryOPStmt>(*it);
            if (!bop || bop->getOpVarNum() != 2)
                continue;
            const SVFVar* op0 = bop->getOpVar(0);
            const SVFVar* op1 = bop->getOpVar(1);
            const ConstIntValVar* c0 = SVFUtil::dyn_cast<ConstIntValVar>(op0);
            const ConstIntValVar* c1 = SVFUtil::dyn_cast<ConstIntValVar>(op1);
            const auto opc = bop->getOpcode();
            std::vector<AffineTerm> base;
            long long k = 0;
            bool handled = false;
            if (opc == BinaryOPStmt::OpCode::Add)
            {
                if (c1 && !c0) { base = analyzeAffine(op0, visited, depth + 1); k = c1->getSExtValue(); handled = true; }
                else if (c0 && !c1) { base = analyzeAffine(op1, visited, depth + 1); k = c0->getSExtValue(); handled = true; }
            }
            else if (opc == BinaryOPStmt::OpCode::Sub)
            {
                if (c1 && !c0) { base = analyzeAffine(op0, visited, depth + 1); k = -c1->getSExtValue(); handled = true; }
            }
            if (handled)
            {
                for (auto& t : base)
                    t.offset += k;
                mergeAffine(res, base, MAX_AFFINE_TERMS);
            }
            else
                mergeAffine(res, {{"O" + std::to_string(var->getId()), 0}}, MAX_AFFINE_TERMS);
        }

    // Phi: union over incoming operands.
    if (var->hasIncomingEdges(SVFStmt::PEDGEK::Phi))
        for (auto it = var->getIncomingEdgesBegin(SVFStmt::PEDGEK::Phi);
             it != var->getIncomingEdgesEnd(SVFStmt::PEDGEK::Phi); ++it)
            if (const PhiStmt* phi = SVFUtil::dyn_cast<PhiStmt>(*it))
                for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
                    mergeAffine(res, analyzeAffine(phi->getOpVar(i), visited, depth + 1),
                                MAX_AFFINE_TERMS);

    // Select: union of the two branches.
    if (var->hasIncomingEdges(SVFStmt::PEDGEK::Select))
        for (auto it = var->getIncomingEdgesBegin(SVFStmt::PEDGEK::Select);
             it != var->getIncomingEdgesEnd(SVFStmt::PEDGEK::Select); ++it)
            if (const SelectStmt* sel = SVFUtil::dyn_cast<SelectStmt>(*it))
            {
                mergeAffine(res, analyzeAffine(sel->getTrueValue(), visited, depth + 1),
                            MAX_AFFINE_TERMS);
                mergeAffine(res, analyzeAffine(sel->getFalseValue(), visited, depth + 1),
                            MAX_AFFINE_TERMS);
            }

    visited.erase(var);

    if (res.empty())
        res.push_back({"O" + std::to_string(var->getId()), 0});
    return res;
}

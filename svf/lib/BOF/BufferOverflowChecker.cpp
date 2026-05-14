//===----- BufferOverflowChecker.cpp -- Detecting Buffer Overflow errors --------------//
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
 * BufferOverflowChecker.cpp
 *
 *  Created on: Nov 10 , 2025
 *      Author: Yaokun Yang
 */

#include "BOF/BufferOverflowChecker.h"
using namespace SVF;
using namespace std;

BufferOverflowChecker::BufferOverflowChecker()
        : heapAllocationHandler(&rangeAnalysis) {         
}

void BufferOverflowChecker::runOnModule(SVFIR* pag)
{
    assert(pag && "PAG must not be null");

    cout << "handling for buffer overflow" << endl;

    // Initialize: add all array nodes to worklist with offset and size
    initialize(pag);

    // Propagate: breadth-first research(BFS)
    propagate(pag);
}

void BufferOverflowChecker::initialize(SVFIR* pag)
{   
    // handle alloca instructions(Stack Objects)
    SVFStmt::SVFStmtSetTy addrStmtSet = pag->getSVFStmtSet(SVFStmt::Addr);
    for (const auto& stmt: addrStmtSet)
    {   
        if(const auto& addrStmt = SVFUtil::dyn_cast<AddrStmt>(stmt)){
            const SVFVar* src = addrStmt->getRHSVar();
            const SVFVar* dst = addrStmt->getLHSVar();
            
            // handle alloca instructions(Stack Objects)
            if(const StackObjVar* stackObjVar = SVFUtil::dyn_cast<StackObjVar>(src)){
                if(rangeAnalysis.analyzeBufferRange(stackObjVar))
                    worklist.push(RangeFlowNode(dst, src, Range(0,0)));
            }
            // handle malloc instructions(Heap Objects)
            else if(const HeapObjVar* heapObjVar = SVFUtil::dyn_cast<HeapObjVar>(src)){
                bool hasBufferRange = rangeAnalysis.analyzeBufferRange(heapObjVar);
                if(!hasBufferRange)
                {
                    if(const CallICFGNode* callNode = SVFUtil::dyn_cast<CallICFGNode>(heapObjVar->getICFGNode()))
                    {
                        Range sizeRange = heapAllocationHandler.analyzeAllocSize(callNode);
                        hasBufferRange = rangeAnalysis.setBufferByteRange(heapObjVar, sizeRange);
                    }
                }
                if(hasBufferRange)
                    worklist.push(RangeFlowNode(dst, src, Range(0,0), true));
            }
        }   
    }
}

void BufferOverflowChecker::propagate(SVFIR* pag)
{
    while (!worklist.empty())
    {
        RangeFlowNode srcNode = worklist.front();
        worklist.pop();

        for(const auto& svfStmt: srcNode.base->getOutEdges())
        {   
            // Gep instruction
            if(const auto& gepStmt = SVFUtil::dyn_cast<GepStmt>(svfStmt))
            {
                // Destination node
                SVFVar* dstVar = gepStmt->getLHSVar();  // gepStmt->getDstNode(); 
                const AccessPath& ap = gepStmt->getAccessPath();
                const AccessPath::IdxOperandPairs& idxOperandPairs = ap.getIdxOperandPairVec();
                
                // s64_t offset = ap.computeConstantOffset();

                Range total_offset = Range(0);
                // if (idxOperandPairs.size() == 0)
                //     return getConstantStructFldIdx();
                for(int i = idxOperandPairs.size() - 1; i >= 0; i--)
                {
                    const SVFVar* var = idxOperandPairs[i].first;
                    const SVFType* type = idxOperandPairs[i].second;

                    Range var_offset = rangeAnalysis.analyzeVarRange(var);
                    if(type == nullptr)
                    {
                        total_offset = Range::add(total_offset, var_offset);
                        continue;
                    }
                    
                    if(srcNode.isHeap){
                        if(SVFUtil::isa<SVFPointerType>(type))
                            total_offset = Range::mul(var_offset, Range(ap.gepSrcPointeeType()->getByteSize()));
                        else
                            total_offset = Range::mul(var_offset, Range(1));
                    }
                    else if(SVFUtil::isa<SVFPointerType>(type))
                        total_offset = Range::add(total_offset, Range::mul(var_offset, Range(ap.getElementNum(ap.gepSrcPointeeType()))));
                    else
                    {
                        const std::vector<u32_t>& so = PAG::getPAG()->getTypeInfo(type)->getFlattenedElemIdxVec();
                        Range type_size = Range(0, so.size()-1);
                        if(!var_offset.isSubset(type_size)){
                            reportBufferOverflowError(dstVar, type, var_offset, type_size);
                            var_offset = Range::meet(var_offset, type_size);
                        }

                        total_offset = Range::join(total_offset, Range(
                            PAG::getPAG()->getFlattenedElemIdx(type, var_offset.getLower()),
                            PAG::getPAG()->getFlattenedElemIdx(type, var_offset.getUpper())
                        ));
                    }
                }
                Range accumulate_offset = Range::add(total_offset, srcNode.accumulate_offset);
                Range buffer_size = rangeAnalysis.getBufferRange(srcNode.parent);

                // printf("Access index: [%lld,%lld], Array size: [%lld,%lld]\n", accumulate_offset.getLower(), accumulate_offset.getUpper(), buffer_size.getLower(), buffer_size.getUpper());

                if(!buffer_size.isBottom() && !accumulate_offset.isSubset(buffer_size)){
                    reportBufferOverflowError(dstVar, nullptr, accumulate_offset, buffer_size, srcNode.isHeap);
                }
                worklist.push(RangeFlowNode(dstVar, srcNode.parent, accumulate_offset, srcNode.isHeap));

            }
            
            // Copy instruction 
            else if(auto *copyStmt = SVFUtil::dyn_cast<CopyStmt>(svfStmt))
            {
                // get destination node
                SVFVar* dstVar = copyStmt->getLHSVar(); // copyStmt->getDstNode();

                // copy offset
                Range accumulate_offset = srcNode.accumulate_offset;
                Range buffer_size = rangeAnalysis.getBufferRange(srcNode.parent);

                if(!buffer_size.isBottom() && !accumulate_offset.isSubset(buffer_size)){
                    reportBufferOverflowError(dstVar, nullptr, accumulate_offset, buffer_size, srcNode.isHeap);
                }

                worklist.push(RangeFlowNode(dstVar, srcNode.parent, accumulate_offset, srcNode.isHeap));
            }

            // Store instruction 
            else if(auto *copyStmt = SVFUtil::dyn_cast<StoreStmt>(svfStmt))
            {
                // get destination node
                SVFVar* dstVar = copyStmt->getLHSVar(); // copyStmt->getDstNode();

                // copy offset
                Range accumulate_offset = srcNode.accumulate_offset;
                Range buffer_size = rangeAnalysis.getBufferRange(srcNode.parent);

                if(!buffer_size.isBottom() && !accumulate_offset.isSubset(buffer_size)){
                    reportBufferOverflowError(dstVar, nullptr, accumulate_offset, buffer_size, srcNode.isHeap);
                }

                worklist.push(RangeFlowNode(dstVar, srcNode.parent, accumulate_offset, srcNode.isHeap));
            }

            // Load instruction
            else if(auto *copyStmt = SVFUtil::dyn_cast<LoadStmt>(svfStmt))
            {
                // get destination node
                SVFVar* dstVar = copyStmt->getLHSVar(); // copyStmt->getDstNode();

                // copy offset
                Range accumulate_offset = srcNode.accumulate_offset;
                Range buffer_size = rangeAnalysis.getBufferRange(srcNode.parent);

                if(!buffer_size.isBottom() && !accumulate_offset.isSubset(buffer_size)){
                    reportBufferOverflowError(dstVar, nullptr, accumulate_offset, buffer_size, srcNode.isHeap);
                }

                worklist.push(RangeFlowNode(dstVar, srcNode.parent, accumulate_offset, srcNode.isHeap));
            }
        }
    }
}


void BufferOverflowChecker::reportBufferOverflowError(const SVFVar* base, const SVFType* type, const Range& offset, const Range& array_size, bool isHeap)
{
    if(!isHeap){
        if(type != nullptr)
            SVFUtil::errs() << "[BufferOverflowChecker] Buffer Overflow Error detected! \n"
                            << "Base: " << base->toString() << "\n"
                            << "Type: " << type->toString() << "\n"
                            << "Buffer index: " << offset.toString() << "\n"
                            << "Buffer size(number of elements): " << array_size.toString() << "\n\n";
        else
            SVFUtil::errs() << "[BufferOverflowChecker] Buffer Overflow Error detected! \n"
                            << "Base: " << base->toString() << "\n"
                            << "Buffer index: " << offset.toString() << "\n"
                            << "Buffer size(number of elements): " << array_size.toString() << "\n\n";
    }
    else{
        if(type != nullptr)
            SVFUtil::errs() << "[BufferOverflowChecker] Buffer Overflow Error detected! \n"
                            << "Base: " << base->toString() << "\n"
                            << "Type: " << type->toString() << "\n"
                            << "Buffer index: " << offset.toString() << "\n"
                            << "Buffer size(bytes): " << array_size.toString() << "\n\n";
        else
            SVFUtil::errs() << "[BufferOverflowChecker] Buffer Overflow Error detected! \n"
                            << "Base: " << base->toString() << "\n"
                            << "Buffer index: " << offset.toString() << "\n"
                            << "Buffer size(bytes): " << array_size.toString() << "\n\n";
    }
                        
}

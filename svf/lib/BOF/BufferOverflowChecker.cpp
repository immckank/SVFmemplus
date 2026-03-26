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
    SVFStmt::SVFStmtSetTy addrStmtSet = pag->getSVFStmtSet(SVFStmt::Addr);
    for (auto stmt: addrStmtSet)
    {   
        if(auto addrStmt = SVFUtil::dyn_cast<AddrStmt>(stmt)){
            const SVFVar* src = addrStmt->getRHSVar();
            const SVFVar* dst = addrStmt->getLHSVar();
            
            // handle alloca instruction with stack obj
            if(const StackObjVar* stackObjVar = SVFUtil::dyn_cast<StackObjVar>(src)){
                rangeAnalysis.analysisBufferRange(stackObjVar);
                worklist.push(RangeFlowNode(dst, src, Range(0,0)));
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

        // cout << srcNode.accumulate_offset.getLower() << " " << srcNode.accumulate_offset.getUpper() << endl;

        for(const auto &svfStmt: srcNode.base->getOutEdges())
        {   
            // Gep instruction
            if(auto *gepStmt = SVFUtil::dyn_cast<GepStmt>(svfStmt))
            {
                // get destination node
                SVFVar* dstVar = gepStmt->getLHSVar();  // gepStmt->getDstNode(); 
                
                Range accumulate_offset = rangeAnalysis.analysisIndexRange(dstVar, gepStmt) + srcNode.accumulate_offset;
                Range buffer_size = rangeAnalysis.getBufferRange(srcNode.parent);

                // printf("Access index: [%lld,%lld], Array size: [%lld,%lld]\n", accumulate_offset.getLower(), accumulate_offset.getUpper(), buffer_size.getLower(), buffer_size.getUpper());

                if(!accumulate_offset.isSubset(buffer_size)){
                    reportBufferOverflowError(dstVar, accumulate_offset, buffer_size);
                }
                worklist.push(RangeFlowNode(dstVar, srcNode.parent, accumulate_offset));

            }
            
            // Copy instruction 
            else if(auto *copyStmt = SVFUtil::dyn_cast<CopyStmt>(svfStmt))
            {
                // get destination node
                SVFVar* dstVar = copyStmt->getLHSVar(); // copyStmt->getDstNode();

                // copy offset
                Range accumulate_offset = srcNode.accumulate_offset;
                Range buffer_size = rangeAnalysis.getBufferRange(srcNode.parent);

                if(!accumulate_offset.isSubset(buffer_size)){
                    reportBufferOverflowError(dstVar, accumulate_offset, buffer_size);
                }

                worklist.push(RangeFlowNode(dstVar, srcNode.parent, accumulate_offset));
            }
        }
    }
}


void BufferOverflowChecker::reportBufferOverflowError(const SVFVar* base, Range offset, Range array_size)
{
    SVFUtil::outs() << "[BufferOverflowChecker] Buffer Overflow Error detected! \n"
                    << "Base: " << base->toString() << "\n"
                    << "Buffer index: [" << offset.getLower() << "," << offset.getUpper() << "]\n"
                    << "Buffer size: [" << array_size.getLower() << "," << array_size.getUpper() << "]\n"
                    << "\n";
}

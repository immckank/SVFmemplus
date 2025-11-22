//===----- AOBChecker.cpp -- Detecting Array Out-of-Bounds--------------//
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
 * AOBChecker.cpp
 *
 *  Created on: Nov 10 , 2025
 *      Author: Yaokun Yang
 */

#include "AOB/AOBChecker.h"
using namespace SVF;
using namespace std;

void AOBChecker::runOnModule(SVFIR* pag)
{
    assert(pag && "PAG must not be null");

    cout << "handling for aob" << endl;

    // Initialize: add all array nodes to worklist with offset and size
    initialize(pag);

    // Propagate: breadth-first research(BFS)
    propagate(pag);
}

void AOBChecker::initialize(SVFIR* pag)
{   
    SVFStmt::SVFStmtSetTy addrStmtSet = pag->getSVFStmtSet(SVFStmt::Addr);
    for (auto stmt: addrStmtSet)
    {   
        if(auto addrStmt = SVFUtil::dyn_cast<AddrStmt>(stmt)){
            const SVFVar* src = addrStmt->getRHSVar();
            const SVFVar* dest = addrStmt->getLHSVar();
            // handle alloca instruction with stack obj
            if(const StackObjVar* stackObjVar = SVFUtil::dyn_cast<StackObjVar>(src)){
                u64_t size = 0;

                // handle case: %arrayidx = alloca [10*i32], align 16
                const SVFType* objType = stackObjVar->getType();
                const StInfo* objInfo = objType->getTypeInfo();
                size = objInfo->getNumOfFlattenElements();
                if(size > 1)
                {
                    this->worklist.push(BFSNode(dest, 0, size));
                    // cout << "add case from type, size = " << size << endl;
                    continue;
                }
            

                // handle case: int a[n] -> %a = alloca i32, i32 %n, align 4
                size = stackObjVar->getNumOfElements();
                if(size > 1)
                {      
                    // cout << "add case from stack, size = " << size << endl;
                    this->worklist.push(BFSNode(dest, 0, size));
                    continue;
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
            }
        }   
    }
}

void AOBChecker::propagate(SVFIR* pag)
{
    while (!worklist.empty())
    {
        BFSNode cur = worklist.front();
        worklist.pop();

        for(const auto &svfStmt: cur.node->getOutEdges())
        {   
            s64_t offset = 0;

            // Gep instruction
            if(auto *gepStmt = SVFUtil::dyn_cast<GepStmt>(svfStmt))
            {
                // get destination node
                SVFVar* dstNode = gepStmt->getLHSVar();  // gepStmt->getDstNode(); 
                
                // calculate offset
                AccessPath ap = gepStmt->getAccessPath();
                offset = cur.offset + ap.computeConstantOffset();
                
                // in-bound checking
                if(offset < 0 || static_cast<u64_t>(offset) >= cur.size)
                    report(dstNode, offset, cur.size);
                
                // add to queue
                this->worklist.push(BFSNode(dstNode, offset, cur.size));

            }
            
            // Copy instruction 
            else if(auto *copyStmt = SVFUtil::dyn_cast<CopyStmt>(svfStmt))
            {
                // get destination node
                SVFVar* dstNode = copyStmt->getLHSVar(); // copyStmt->getDstNode();

                // copy offset
                offset = cur.offset;

                // in-bound checking
                if(offset < 0 || static_cast<u64_t>(offset) >= cur.size)
                    report(dstNode, offset, cur.size);

                // add to queue
                this->worklist.push(BFSNode(dstNode, offset, cur.size));
            }

            // cout << "offset = " << offset << endl;
        }
    }
}


void AOBChecker::report(const SVFVar* base, s64_t fldIdx, u64_t arraySize)
{
    cout << "[AOBChecker][BFS] Array Out-of-Bounds detected! "
         << "Base: " << base->toString()
         << ", Access index: " << fldIdx
         << ", Array size: " << arraySize
         << endl;
}

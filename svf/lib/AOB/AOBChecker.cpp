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

void AOBchecker::runOnModule(SVFIR* pag)
{
    assert(pag && "PAG must not be null");

    cout << "checking for array index out of bound!" << endl;
    cout << (pag == NULL) << endl;

    // Initialize: add all array nodes to worklist with offset and size
    for (auto it = pag->begin(); it != pag->end(); ++it)
    {
        // const NodeID nodeID = it->first;
        const SVFVar *node = it->second;
        cout << node->getValueName() << endl;
        // Get gep nodes
        if(auto *gepNode = SVFUtil::dyn_cast<GepValVar>(node))
        {
            cout << "there is a gep val var!" << endl;
            // Get array nodes
            const SVFType *nodeType = gepNode->getType(); 
            if(nodeType->isArrayTy()){
                // Get size of array 
                s64_t size = 0;
                const ValVar *baseNode = gepNode->getBaseNode();

                // Memory on stack created by alloca instruction
                // for example: int a[10] -> %a = alloca [10*i32], align 16
                if(auto *stackObjVar = SVFUtil::dyn_cast<StackObjVar>(baseNode))
                {
                    const SVFType *arrayType = stackObjVar->getType();
                    const StInfo *arrayInfo = arrayType->getTypeInfo();
                    size = arrayInfo->getNumOfFlattenElements();
                }

                // Memory on heap created by malloc instruction
                else if(auto *heapObjVar = SVFUtil::dyn_cast<HeapObjVar>(baseNode)){
                    const SVFType *arrayType = heapObjVar->getType();
                    const StInfo *arrayInfo = arrayType->getTypeInfo();
                    size = arrayInfo->getNumOfFlattenElements();
                }

                // Add to worklist
                if(size > 0)
                    this->worklist.push(BFSNode(gepNode, 0, size));

                cout << "size = " << size << endl;
            }

        }
    }

    // Propagate: breadth-first research(BFS)
    propagate(pag);
}

void AOBchecker::propagate(SVFIR* pag)
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
                offset = cur.offset + ap.getConstantStructFldIdx();
                
                // in-bound checking
                if(offset < 0 || offset >= cur.size)
                    report(dstNode, offset, cur.size);
                
                // add to queue
                this->worklist.push(BFSNode(dstNode, offset, cur.size));

            }
            
            // Copy instruction 
            else if(auto *copyStmt = SVFUtil::dyn_cast<CopyStmt>(svfStmt))
            {
                // get destination node
                SVFVar* dstNode = copyStmt->getLHSVar(); // copyStmt->getDstNode();

                // calculate offset
                offset = cur.offset;

                // in-bound checking
                if(offset < 0 || offset >= cur.size)
                    report(dstNode, offset, cur.size);

                // add to queue
                this->worklist.push(BFSNode(dstNode, offset, cur.size));
            }

            cout << "offset = " << offset << endl;
        }
    }
}


void AOBchecker::report(const SVFVar* base, s64_t fldIdx, s64_t arraySize)
{
    cout << "[AOBChecker][BFS] Array Out-of-Bounds detected! "
         << "Base: " << base->toString()
         << ", Access index: " << fldIdx
         << ", Array size: " << arraySize
         << endl;
}

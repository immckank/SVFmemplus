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
    bufferRanges[stackObjVar] = Range::UNDEFINED;
};
         


Range RangeAnalysis::analysisIndexRange(const SVFVar* array, const GepStmt* gepStmt){
    // calculate offset
    AccessPath ap = gepStmt->getAccessPath();
    s64_t offset = ap.computeConstantOffset();
    return Range(offset, offset);
};



Range RangeAnalysis::getBufferRange(const SVFVar* buffer){
    auto it = bufferRanges.find(buffer);
    if (it != bufferRanges.end()) {
        return it->second;
    }
    return Range::UNDEFINED;
};


            
Range RangeAnalysis::getIndexRange(const SVFVar* index){
    auto it = indexRanges.find(index);
    if (it != indexRanges.end()) {
        return it->second;
    }
    return Range::UNDEFINED;
};
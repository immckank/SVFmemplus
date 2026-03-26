//===- Range.h -- Record Range SVF Vars -------------------------------//
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
 * Range.h
 *
 *  Created on: MAR 15, 2025
 *      Author: Yaokun Yang
 */


#include "BOF/Range.h"
using namespace SVF;


// ===== 常量定义 =====
const Range::BoundType Range::INF = std::numeric_limits<Range::BoundType>::max();
const Range::BoundType Range::NINF = std::numeric_limits<Range::BoundType>::min();
const Range Range::ZERO = Range(0, 0);
const Range Range::UNDEFINED = Range(Range::NINF, Range::INF);

// ===== 构造函数 =====
Range::Range() : lower(NINF), upper(INF) {}

Range::Range(BoundType l, BoundType u)
    : lower(l), upper(u) {}

// ===== getter =====
Range::BoundType Range::getLower() const {
    return lower;
}

Range::BoundType Range::getUpper() const {
    return upper;
}

// ===== 运算符 =====
Range Range::operator+(const Range& other) const {
    return Range(
        safeAdd(lower, other.lower),
        safeAdd(upper, other.upper)
    );
}

Range& Range::operator+=(const Range& other) {
    *this = *this + other;
    return *this;
}

bool Range::isSubset(const Range& other){
    return this->lower >= other.getLower() && this->upper <= other.getUpper(); 
}

// ===== 工具函数 =====
bool Range::isUndefined() const {
    return lower == NINF && upper == INF;
}

// ===== 内部函数 =====
Range::BoundType Range::safeAdd(BoundType a, BoundType b) {
    if (a == INF || b == INF) 
        return INF;
    if (a == NINF || b == NINF) 
        return NINF;
    if (b > 0 && a > INF - b) 
        return INF;
    if (b < 0 && a < NINF - b) 
        return NINF;

    return a + b;
}


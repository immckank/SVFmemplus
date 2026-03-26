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

 // Range.h
#ifndef RANGE_H
#define RANGE_H

#include <limits>

namespace SVF {
    class Range {
        public:
            using BoundType = signed long long;

            // 常量
            static const BoundType INF;
            static const BoundType NINF;
            static const Range ZERO;
            static const Range UNDEFINED;

            // 构造函数
            Range();
            Range(BoundType l, BoundType u);

            // getter
            BoundType getLower() const;
            BoundType getUpper() const;

            // 运算符
            Range operator+(const Range& other) const;
            Range& operator+=(const Range& other);
            bool isSubset(const Range& other);

            // 工具函数
            bool isUndefined() const;

        private:
            BoundType lower;
            BoundType upper;

            static BoundType safeAdd(BoundType a, BoundType b);
    };
}

#endif /* RANGE_H_ */
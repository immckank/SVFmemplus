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
#include <algorithm>

namespace SVF {
    class Range {
        public:
            using BoundType = signed long long;

            // Constant
            static const BoundType INF;
            static const BoundType NINF;
            static const Range TOP; // [NINF, INF]
            static const Range BOTTOM; // [INF, NINF]

            // constructor
            Range();
            Range(BoundType s);
            Range(BoundType l, BoundType u);

            // getter
            BoundType getLower() const;
            BoundType getUpper() const;
            
            // ===== Lattice Alergbra =====
            // Arithmetic Operations
            static Range add(const Range& lhs, const Range& rhs);
            static Range sub(const Range& lhs, const Range& rhs);
            static Range mul(const Range& lhs, const Range& rhs);
            static Range div(const Range& lhs, const Range& rhs);
            static Range mod(const Range& lhs, const Range& rhs);
            static Range negate(const Range& operand);

            // Bitwise Operations
            static Range bit_and(const Range& lhs, const Range& rhs);
            static Range bit_or(const Range& lhs, const Range& rhs);
            static Range bit_xor(const Range& lhs, const Range& rhs);
            static Range bit_not(const Range& operand);

            // Shift Operations
            static Range shl(const Range& lhs, const Range& rhs);
            static Range shr(const Range& lhs, const Range& rhs);

            // ===== Compare Operations =====
            static bool eq(const Range& lhs, const Range& rhs);
            static bool ne(const Range& lhs, const Range& rhs);
            static bool lt(const Range& lhs, const Range& rhs);
            static bool le(const Range& lhs, const Range& rhs);
            static bool gt(const Range& lhs, const Range& rhs);
            static bool ge(const Range& lhs, const Range& rhs);

            // ===== Logical Operations =====
            static Range logical_not(const Range& operand);
            static Range logical_and(const Range& lhs, const Range& rhs);
            static Range logical_or(const Range& lhs, const Range& rhs);

            // ===== Set Operation =====
            static Range join(const Range& lhs, const Range& rhs); // Union
            static Range meet(const Range& lhs, const Range& rhs); // Intersection

            // select (?:)
            static Range select(const Range& cond, const Range& t, const Range& f);

            // ===== tool =====
            bool isTop() const;
            bool isBottom() const;
            bool isConstant() const;
            bool isSubset(const Range& other) const;
            

        private:
            BoundType lower;
            BoundType upper;

            static BoundType safeAdd(BoundType a, BoundType b);
            static BoundType safeMul(BoundType a, BoundType b);
    };
}

#endif /* RANGE_H_ */
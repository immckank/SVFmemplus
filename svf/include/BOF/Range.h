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
#include <sstream>

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

            // ===== tool =====
            bool isTop() const;
            bool isBottom() const;
            bool isConstant() const;
            bool isSubset(const Range& other) const;
            std::string toString() const;

            
            // ===== Lattice Alergbra =====
            /**
             * @brief Computes the result range of arithmetic operations between two ranges.
             *
             * These functions analyze the possible range of results when performing
             * addition, subtraction, multiplication, division, or modulo operations
             * on static value ranges.
             *
             * @param lhs The left-hand side range operand.
             * @param rhs The right-hand side range operand (for unary negate, this is unused).
             * @param operand The operand range for negation.
             *
             * @return A Range representing the possible result values of the corresponding
             *         arithmetic operation. The result is derived based on static range analysis.
             *
             * @note Division and modulo operations may return an invalid range if the rhs
             *       range contains zero (division by zero). The caller should handle such cases.
             */
            static Range add(const Range& lhs, const Range& rhs);
            static Range sub(const Range& lhs, const Range& rhs);
            static Range mul(const Range& lhs, const Range& rhs);
            static Range div(const Range& lhs, const Range& rhs);
            static Range mod(const Range& lhs, const Range& rhs);
            static Range negate(const Range& operand);

            /**
             * @brief Computes the result range of bitwise operations between two ranges.
             *
             * These functions analyze the possible range of results when performing
             * bitwise AND, OR, or XOR operations on static value ranges.
             *
             * @param lhs The left-hand side range operand.
             * @param rhs The right-hand side range operand (for unary bit_not, this is unused).
             * @param operand The operand range for bitwise NOT.
             *
             * @return A Range representing the possible result values of the corresponding
             *         bitwise operation. The result is derived based on static range analysis.
             *
             * @note bit_and typically produces a more constrained range (clears bits).
             *       bit_or and bit_xor may produce wider ranges (sets or flips bits).
             */
            static Range bit_and(const Range& lhs, const Range& rhs);
            static Range bit_or(const Range& lhs, const Range& rhs);
            static Range bit_xor(const Range& lhs, const Range& rhs);
            static Range bit_not(const Range& operand);

            /**
             * @brief Computes the result range of shift operations between two ranges.
             *
             * These functions analyze the possible range of results when performing
             * logical left shift (shl), logical right shift (lshr), or arithmetic
             * right shift (ashr) operations on static value ranges.
             *
             * @param lhs The left-hand side range operand (value to be shifted).
             * @param rhs The right-hand side range operand (shift amount).
             *
             * @return A Range representing the possible result values of the corresponding
             *         shift operation. The result is derived based on static range analysis.
             *
             * @note The result is typically constrained by the bit width of the underlying type.
             *       Shift amounts that exceed the bit width may be truncated (behavior depends
             *       on the target language semantics, e.g., C/C++ UB for overshift).
             *       lshr fills vacated bits with zero; ashr preserves the sign bit.
             */
            static Range shl(const Range& lhs, const Range& rhs);
            static Range lshr(const Range& lhs, const Range& rhs);
            static Range ashr(const Range& lhs, const Range& rhs);

            /**
             * @brief Performs comparison between two ranges.
             *
             * These functions determine whether the relationship between two ranges
             * is definitely true, definitely false, or uncertain based on static
             * value range analysis.
             *
             * @param lhs The left-hand side range operand.
             * @param rhs The right-hand side range operand.
             *
             * @return true  If the comparison is definitely true for all values in both ranges.
             * @return false Otherwise (the comparison may be false or uncertain).
             *
             * @note These functions return conservative results:
             *       - eq returns true only if the ranges overlap exactly and are singletons.
             *       - ne returns true if the ranges do not overlap at all.
             *       - lt/le/gt/ge return true only if the entire lhs range satisfies
             *         the relation with respect to the entire rhs range.
             *       When the relationship cannot be statically determined, false is returned.
             */
            static bool eq(const Range& lhs, const Range& rhs);
            static bool ne(const Range& lhs, const Range& rhs);
            static bool lt(const Range& lhs, const Range& rhs);
            static bool le(const Range& lhs, const Range& rhs);
            static bool gt(const Range& lhs, const Range& rhs);
            static bool ge(const Range& lhs, const Range& rhs);

            /**
             * @brief Computes the result range of logical operations on ranges.
             *
             * These functions analyze the possible range of results when performing
             * logical NOT (&& !), logical AND (&&), or logical OR (||) operations
             * on static value ranges. In C/C++, logical operations yield boolean
             * results (0 for false, 1 for true).
             *
             * @param operand The operand range for logical NOT.
             * @param lhs The left-hand side range operand for logical AND/OR.
             * @param rhs The right-hand side range operand for logical AND/OR.
             *
             * @return A Range representing the possible result values:
             *         - For logical_not: returns Range(0, 0) if operand is definitely true,
             *           Range(1, 1) if operand is definitely false, otherwise Range(0, 1).
             *         - For logical_and: returns Range(1, 1) only if both lhs and rhs
             *           are definitely true (non-zero); returns Range(0, 0) if either is
             *           definitely false (zero); otherwise returns Range(0, 1).
             *         - For logical_or: returns Range(0, 0) only if both lhs and rhs
             *           are definitely false; returns Range(1, 1) if either is definitely
             *           true; otherwise returns Range(0, 1).
             *
             * @note These functions assume C/C++ semantics where any non-zero value is
             *       considered "true" and zero is considered "false". The returned range
             *       represents the boolean outcome (0 or 1), not the original operand values.
             */
            static Range logical_not(const Range& operand);
            static Range logical_and(const Range& lhs, const Range& rhs);
            static Range logical_or(const Range& lhs, const Range& rhs);

            /**
             * @brief Performs set operations on ranges.
             *
             * These functions compute the union (join) and intersection (meet) of
             * two value ranges, treating ranges as sets of integers.
             *
             * @param lhs The left-hand side range operand.
             * @param rhs The right-hand side range operand.
             *
             * @return A Range representing the result of the set operation:
             *         - join returns the smallest range that covers all values from
             *           both lhs and rhs (union). If the ranges are disjoint, the
             *           result is a range spanning from min(lhs.lb, rhs.lb) to
             *           max(lhs.ub, rhs.ub).
             *         - meet returns the intersection of lhs and rhs. If the ranges
             *           do not overlap, an invalid Range (empty set) is returned.
             *
             * @note join may over-approximate (include values not in either range)
             *       when the ranges are disjoint, as Range assumes contiguous intervals.
             */
            static Range join(const Range& lhs, const Range& rhs); // Union
            static Range meet(const Range& lhs, const Range& rhs); // Intersection

            /**
             * @brief Computes the result range of a ternary conditional expression (cond ? t : f).
             *
             * This function analyzes the possible range of results when evaluating
             * a conditional expression based on static value ranges of the condition,
             * true-branch, and false-branch operands.
             *
             * @param cond The range of the condition expression.
             * @param t    The range of the true-branch expression (executed when cond is true).
             * @param f    The range of the false-branch expression (executed when cond is false).
             *
             * @return A Range representing the possible result values:
             *         - If cond is definitely true (non-zero), returns Range(t).
             *         - If cond is definitely false (zero), returns Range(f).
             *         - Otherwise (cond may be true or false), returns the union (join)
             *           of t and f.
             *
             * @note This function assumes C/C++ semantics where any non-zero value is
             *       considered "true" and zero is considered "false".
             */
            static Range select(const Range& cond, const Range& t, const Range& f);
            

        private:
            BoundType lower;
            BoundType upper;

            static BoundType safeAdd(BoundType a, BoundType b);
            static BoundType safeMul(BoundType a, BoundType b);
    };
}

#endif /* RANGE_H_ */
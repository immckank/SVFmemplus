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


// ===== Constant Definition =====
const Range::BoundType Range::INF = std::numeric_limits<Range::BoundType>::max();
const Range::BoundType Range::NINF = std::numeric_limits<Range::BoundType>::min();

const Range Range::TOP  = Range(NINF, INF);
const Range Range::BOTTOM = Range(INF, NINF);


// ===== Constructor =====
Range::Range() : lower(NINF), upper(INF) {}

Range::Range(BoundType s): lower(s), upper(s) {}

Range::Range(BoundType l, BoundType u)
    : lower(l), upper(u) {}


// ===== getter =====
Range::BoundType Range::getLower() const {
    return lower;
}

Range::BoundType Range::getUpper() const {
    return upper;
}


// ===== tool =====
bool Range::isBottom() const {
    return lower > upper;
}

bool Range::isTop() const {
    return lower == NINF && upper == INF;
}

bool Range::isConstant() const {
    return lower == upper;
}

bool Range::isSubset(const Range& other) const {
    return lower >= other.getLower() 
    && upper <= other.getUpper(); 
}

Range::BoundType Range::safeAdd(BoundType lhs, BoundType rhs) {
    if (rhs > 0 && lhs > INF - rhs) 
        return INF;
    else if (rhs < 0 && lhs < NINF - rhs) 
        return NINF;
    else
        return lhs + rhs;
}

Range::BoundType Range::safeMul(BoundType lhs, BoundType rhs) {
    if (lhs == 0 || rhs == 0) 
        return 0;
    else if (lhs > 0 && rhs > 0 && lhs > INF / rhs) 
        return INF;
    else if (lhs < 0 && rhs < 0 && lhs < INF / rhs) 
        return INF;
    else if (lhs > 0 && rhs < 0 && rhs < NINF / lhs) 
        return NINF;
    else if (lhs < 0 && rhs > 0 && lhs < NINF / rhs) 
        return NINF;
    else
        return lhs * rhs;
}

// ===== Lattice Alergbra =====
// Arithmetic Operations
Range Range::add(const Range& lhs, const Range& rhs) {
    if (lhs.isBottom() || rhs.isBottom()) 
        return BOTTOM;
    return Range(
        safeAdd(lhs.lower, rhs.lower),
        safeAdd(lhs.upper, rhs.upper)
    );
}

Range Range::sub(const Range& lhs, const Range& rhs) {
    if (lhs.isBottom() || rhs.isBottom()) 
        return BOTTOM;
    else return Range(
        safeAdd(lhs.lower, -rhs.upper),
        safeAdd(lhs.upper, -rhs.lower)
    );
}

Range Range::mul(const Range& lhs, const Range& rhs) {
    if (lhs.isBottom() || rhs.isBottom()) 
        return BOTTOM;

    BoundType candidates[4] = {
        safeMul(lhs.lower, rhs.lower),
        safeMul(lhs.lower, rhs.upper),
        safeMul(lhs.upper, rhs.lower),
        safeMul(lhs.upper, rhs.upper)
    };

    return Range(
        *std::min_element(candidates, candidates + 4),
        *std::max_element(candidates, candidates + 4)
    );
}

Range Range::div(const Range& lhs, const Range& rhs) {
    if (lhs.isBottom() || rhs.isBottom()) 
        return BOTTOM;
    if (rhs.lower <= 0 && rhs.upper >= 0) 
        return TOP;

    BoundType candidates[4] = {
        lhs.lower / rhs.lower,
        lhs.lower / rhs.upper,
        lhs.upper / rhs.lower,
        lhs.upper / rhs.upper
    };

    return Range(
        *std::min_element(candidates, candidates + 4),
        *std::max_element(candidates, candidates + 4)
    );
}

Range Range::mod(const Range& lhs, const Range& rhs) {
    if (lhs.isBottom() || rhs.isBottom()) 
        return BOTTOM;
    if (rhs.lower <= 0 && rhs.upper >= 0)
        return TOP;

    BoundType maxAbs = std::max(std::abs(rhs.lower), std::abs(rhs.upper));
    return Range(-maxAbs + 1, maxAbs - 1);
}

Range Range::negate(const Range& operand) {
    if (operand.isBottom()) 
        return BOTTOM;
    else
        return Range(-operand.upper, -operand.lower);
}

// Bitwise Operations
Range Range::bit_and(const Range& lhs, const Range& rhs) { 
    return TOP; 
}

Range Range::bit_or(const Range& lhs, const Range& rhs) { 
    return TOP;
}

Range Range::bit_xor(const Range& lhs, const Range& rhs) { 
    return TOP;
}

Range Range::bit_not(const Range& operand) {
    if (operand.isBottom()) 
        return BOTTOM;
    else
        return TOP;
}

// Shift Operations
Range Range::shl(const Range& lhs, const Range& rhs) {
    if (lhs.isBottom() || rhs.isBottom()) 
        return BOTTOM;
    else if (rhs.lower < 0) 
        return TOP;
    else
        return Range(
            safeMul(lhs.lower, 1LL << rhs.lower),
            safeMul(lhs.upper, 1LL << rhs.upper)
        );
}

Range Range::shr(const Range& a, const Range& rhs) {
    if (a.isBottom() || rhs.isBottom()) 
        return BOTTOM;
    else
        return TOP;
}

// ===== Compare Operations =====
bool Range::eq(const Range& lhs, const Range& rhs) {
    if (lhs.isBottom() && rhs.isBottom()) 
        return true;
    else if (lhs.isTop() && rhs.isTop()) 
        return true;
    else
        return lhs.lower == rhs.lower
            && lhs.upper == rhs.upper;
}

bool Range::ne(const Range& lhs, const Range& rhs) {
    return !eq(lhs, rhs);
}

bool Range::lt(const Range& lhs, const Range& rhs) {
    if(lhs.isBottom() && rhs.isBottom())
        return false;
    else if(lhs.isTop() && rhs.isTop())
        return false;
    else
        return lhs.lower > rhs.lower
            && lhs.upper < rhs.upper;
}

bool Range::le(const Range& lhs, const Range& rhs) {
    if(lhs.isBottom() && rhs.isBottom())
        return true;
    else if(lhs.isTop() && rhs.isTop())
        return true;
    else
        return lhs.lower >= rhs.lower
            && lhs.upper <= rhs.upper;
}

bool Range::gt(const Range& lhs, const Range& rhs) { 
    return lt(rhs, lhs); 
}

bool Range::ge(const Range& lhs, const Range& rhs) { 
    return le(rhs, lhs); 
}

// ===== Logical Operations =====
Range Range::logical_not(const Range& operand) {
    if (operand.lower == 0 && operand.upper == 0) 
        return Range(1);
    else if (operand.lower > 0 || operand.upper < 0) 
        return Range(0);
    else
        return Range(1);
}

Range Range::logical_and(const Range& lhs, const Range& rhs) {
    return Range(
        (lhs.lower && rhs.lower) ? 1 : 0,
        (lhs.upper && rhs.upper) ? 1 : 1
    );
}

Range Range::logical_or(const Range& lhs, const Range& rhs) {
    return Range(
        (lhs.lower || rhs.lower) ? 1 : 0,
        (lhs.upper || rhs.upper) ? 1 : 1
    );
}

Range Range::join(const Range& lhs, const Range& rhs) {
    if (lhs.isBottom()) 
        return rhs;
    else if (rhs.isBottom()) 
        return lhs;
    else 
        return Range(
            std::min(lhs.lower, rhs.lower),
            std::max(lhs.upper, rhs.upper)
        );
}

Range Range::meet(const Range& lhs, const Range& rhs) {
    if (lhs.isBottom() || rhs.isBottom()) 
        return BOTTOM;
    else
        return Range(
        std::max(lhs.lower, rhs.lower),
        std::min(lhs.upper, rhs.upper)
    );
}

Range Range::select(const Range& cond, const Range& t, const Range& f) {
    if (cond.lower == 1 && cond.upper == 1) return t;
    if (cond.lower == 0 && cond.upper == 0) return f;
    return join(t, f);
}



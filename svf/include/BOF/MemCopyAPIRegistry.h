//===- MemCopyAPIRegistry.h -- Memory-copy / string API overflow rules --------//
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
 * MemCopyAPIRegistry.h
 *
 *  Created on: JUN 04, 2026
 *      Author: Yaokun Yang
 */

#ifndef MEM_COPY_API_REGISTRY_H
#define MEM_COPY_API_REGISTRY_H

#include <map>
#include <string>
#include <vector>

namespace SVF
{

/**
 * @brief A single buffer-overflow check rule for a memory-copy / string API.
 *
 * Each rule says: argument @c bufArgIdx points to a buffer that is accessed for
 * a length given by argument @c lenArgIdx (length unit = bytes). A function may
 * have multiple rules (e.g. memcpy checks both dst write and src read).
 *
 * @c lenArgIdx == -1 means the length is implicit (e.g. strcpy => strlen(src)),
 * handled conservatively by the checker.
 */
struct MemCopyRule
{
    int bufArgIdx;
    int lenArgIdx;
};

/**
 * @brief BOF-facing wrapper; rules are defined in SaberMemTransferAPI.
 */
class MemCopyAPIRegistry
{
public:
    MemCopyAPIRegistry();

    /// @return the check rules for @p funcName, or nullptr if not a memcopy API.
    const std::vector<MemCopyRule>* getRules(const std::string& funcName) const;

    /// strcpy/strcat-like functions: length must be derived from the source
    /// string (no explicit length argument).
    bool isStrCopyLike(const std::string& funcName) const;

private:
    mutable std::vector<MemCopyRule> rulesCache;
};

} // namespace SVF

#endif /* MEM_COPY_API_REGISTRY_H */

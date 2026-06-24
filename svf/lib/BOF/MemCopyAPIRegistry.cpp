//===- MemCopyAPIRegistry.cpp -- Memory-copy / string API overflow rules ------//
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
 * MemCopyAPIRegistry.cpp
 *
 * Thin BOF wrapper over SaberMemTransferAPI (single source of truth).
 */

#include "BOF/MemCopyAPIRegistry.h"
#include "SABER/SaberMemTransferAPI.h"

using namespace SVF;

MemCopyAPIRegistry::MemCopyAPIRegistry() = default;

const std::vector<MemCopyRule>* MemCopyAPIRegistry::getRules(const std::string& funcName) const
{
    rulesCache.clear();
    const std::vector<MemAccessRule> accessRules =
        SaberMemTransferAPI::getAPI()->getAccessRules(funcName);
    if (accessRules.empty())
        return nullptr;

    for (const MemAccessRule& r : accessRules)
        rulesCache.push_back(MemCopyRule{r.bufArgIdx, r.lenArgIdx});

    return &rulesCache;
}

bool MemCopyAPIRegistry::isStrCopyLike(const std::string& funcName) const
{
    return SaberMemTransferAPI::getAPI()->isStrCopyLike(funcName);
}

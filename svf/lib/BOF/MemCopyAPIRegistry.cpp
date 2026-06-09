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
 *  Created on: JUN 04, 2026
 *      Author: Yaokun Yang
 */

#include "BOF/MemCopyAPIRegistry.h"

using namespace SVF;

MemCopyAPIRegistry::MemCopyAPIRegistry()
{
    // ===== Explicit-length copies: {bufArgIdx, lenArgIdx} =====
    // memcpy(dst, src, n) / memmove(dst, src, n):
    //   dst is written for n bytes, src is read for n bytes -> two rules.
    rules["memcpy"]        = {{0, 2}, {1, 2}};
    rules["memmove"]       = {{0, 2}, {1, 2}};
    rules["__memcpy_chk"]  = {{0, 2}, {1, 2}};
    rules["__memmove_chk"] = {{0, 2}, {1, 2}};
    rules["wmemcpy"]       = {{0, 2}, {1, 2}};
    rules["wmemmove"]      = {{0, 2}, {1, 2}};

    // memset(dst, c, n): dst written for n bytes.
    rules["memset"]        = {{0, 2}};
    rules["__memset_chk"]  = {{0, 2}};
    rules["wmemset"]       = {{0, 2}};
    rules["bzero"]         = {{0, 1}};

    // bcopy(src, dst, n): note the argument order differs from memcpy.
    rules["bcopy"]         = {{0, 2}, {1, 2}};

    // strncpy(dst, src, n) / strncat / __strncpy_chk: dst written up to n bytes.
    rules["strncpy"]       = {{0, 2}};
    rules["__strncpy_chk"] = {{0, 2}};
    rules["strncat"]       = {{0, 2}};
    rules["__strncat_chk"] = {{0, 2}};

    // snprintf(dst, n, fmt, ...): dst written up to n bytes.
    rules["snprintf"]      = {{0, 1}};
    rules["__snprintf_chk"]= {{0, 1}};
    rules["vsnprintf"]     = {{0, 1}};

    // ===== Bounds-checked "secure" variants (C11 Annex K / Huawei securec) =====
    // These are pervasive in OpenHarmony / HDF code. Their signatures put the
    // *copy length* in a later argument than the classic APIs:
    //   errno_t memcpy_s (void *dest, size_t destMax, const void *src, size_t count);
    //   errno_t memmove_s(void *dest, size_t destMax, const void *src, size_t count);
    //   errno_t memset_s (void *dest, size_t destMax, int c,           size_t count);
    //   errno_t strncpy_s(char *dest, size_t destMax, const char *src, size_t count);
    //   errno_t strncat_s(char *dest, size_t destMax, const char *src, size_t count);
    // The number of bytes actually written/read is `count`, so the buffer
    // (arg0) / source (arg2) are checked against arg3 (count).
    rules["memcpy_s"]      = {{0, 3}, {2, 3}};
    rules["memmove_s"]     = {{0, 3}, {2, 3}};
    rules["memset_s"]      = {{0, 3}};
    rules["strncpy_s"]     = {{0, 3}};
    rules["strncat_s"]     = {{0, 3}};

    // ===== Implicit-length copies (length derived from source string) =====
    // These have no explicit length argument; the buffer arg is arg0, and the
    // length is the strlen of the source (arg1), handled conservatively by the
    // checker. lenArgIdx == -1 marks the implicit length.
    strCopyLike["strcpy"]      = true;
    strCopyLike["__strcpy_chk"]= true;
    strCopyLike["strcat"]      = true;
    strCopyLike["__strcat_chk"]= true;
    strCopyLike["stpcpy"]      = true;

    for (const auto& it : strCopyLike)
        rules[it.first] = {{0, -1}};
}

const std::vector<MemCopyRule>* MemCopyAPIRegistry::getRules(const std::string& funcName) const
{
    auto it = rules.find(funcName);
    if (it != rules.end())
        return &it->second;
    return nullptr;
}

bool MemCopyAPIRegistry::isStrCopyLike(const std::string& funcName) const
{
    auto it = strCopyLike.find(funcName);
    return it != strCopyLike.end() && it->second;
}

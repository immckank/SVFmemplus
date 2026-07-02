//===- SaberMemTransferAPI.cpp -- Memory copy/set API for checkers ----------===//
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
 * SaberMemTransferAPI.cpp
 *
 * Shared memory-transfer API table (ei_pairs style, mirrors SaberCheckerAPI).
 */

#include "SABER/SaberMemTransferAPI.h"
#include "SABER/SaberSemanticRules.h"
#include <cassert>
#include <cstdio>
#include <set>

using namespace SVF;

SaberMemTransferAPI* SaberMemTransferAPI::mtAPI = nullptr;

namespace
{

/// spec template id constants (must match spec_templates[] order)
enum SpecId : u16_t
{
    SID_MEMCPY = 0,
    SID_BCOPY,
    SID_MEMCPY_S,
    SID_MEMSET,
    SID_MEMSET_S,
    SID_BZERO,
    SID_STRN_EXPLICIT,
    SID_STR_IMPLICIT,
    SID_STRCAT_IMPLICIT,
    SID_SNPRINTF,
    SID_MEMCCPY,
    SID_USER_COPY,
    SID_NUM_SPECS
};

static const MemAccessRule rules_memcpy[] = {{0, 2}, {1, 2}};
static const MemAccessRule rules_bcopy[] = {{1, 2}, {0, 2}};
static const MemAccessRule rules_memcpy_s[] = {{0, 3}, {2, 3}};
static const MemAccessRule rules_memset[] = {{0, 2}};
static const MemAccessRule rules_memset_s[] = {{0, 3}};
static const MemAccessRule rules_bzero[] = {{0, 1}};
static const MemAccessRule rules_strn[] = {{0, 2}};
static const MemAccessRule rules_strn_cat[] = {{0, 2}};
static const MemAccessRule rules_str_implicit[] = {{0, -1}};
static const MemAccessRule rules_snprintf[] = {{0, 1}};
static const MemAccessRule rules_memccpy[] = {{0, 3}, {1, 3}};

static const SaberMemTransferAPI::TransferSpec spec_templates[SID_NUM_SPECS] =
{
    /// SID_MEMCPY: memcpy(dst, src, n)
    {
        SaberMemTransferAPI::TK_COPY,
        {0, 1, 2, true},
        rules_memcpy, 2,
        false, 0
    },
    /// SID_BCOPY: bcopy(src, dst, n)
    {
        SaberMemTransferAPI::TK_COPY,
        {1, 0, 2, true},
        rules_bcopy, 2,
        false, 0
    },
    /// SID_MEMCPY_S: memcpy_s(dest, destMax, src, count)
    {
        SaberMemTransferAPI::TK_COPY,
        {0, 2, 3, true},
        rules_memcpy_s, 2,
        false, 0
    },
    /// SID_MEMSET
    {
        SaberMemTransferAPI::TK_SET,
        {0, 0, 2, false},
        rules_memset, 1,
        false, 0
    },
    /// SID_MEMSET_S: memset_s(dest, destMax, c, count)
    {
        SaberMemTransferAPI::TK_SET,
        {0, 0, 3, false},
        rules_memset_s, 1,
        false, 0
    },
    /// SID_BZERO
    {
        SaberMemTransferAPI::TK_SET,
        {0, 0, 1, false},
        rules_bzero, 1,
        false, 0
    },
    /// SID_STRN_EXPLICIT: strncpy / strncat / *_s
    {
        SaberMemTransferAPI::TK_STRING_COPY,
        {0, 0, 2, false},
        rules_strn, 1,
        false, 0
    },
    /// SID_STR_IMPLICIT: strcpy / stpcpy
    {
        SaberMemTransferAPI::TK_STRING_COPY,
        {0, 0, -1, false},
        rules_str_implicit, 1,
        true, 1
    },
    /// SID_STRCAT_IMPLICIT
    {
        SaberMemTransferAPI::TK_STRING_CAT,
        {0, 0, -1, false},
        rules_str_implicit, 1,
        true, 1
    },
    /// SID_SNPRINTF
    {
        SaberMemTransferAPI::TK_FORMAT_WRITE,
        {0, 0, 1, false},
        rules_snprintf, 1,
        false, 0
    },
    /// SID_MEMCCPY
    {
        SaberMemTransferAPI::TK_COPY,
        {0, 1, 3, false},
        rules_memccpy, 2,
        false, 0
    },
    /// SID_USER_COPY
    {
        SaberMemTransferAPI::TK_USER_COPY,
        {0, 1, 2, false},
        rules_memcpy, 2,
        false, 0
    },
};

struct mt_ei_pair
{
    const char* n;
    u16_t specId;
};

} // anonymous namespace

// Each (name, specId) pair is inserted into the map.
// All entries sharing the same specId should occur together (for error detection).
static const mt_ei_pair mt_pairs[] =
{
    /// ===== SID_MEMCPY (memcpy / memmove / llvm intrinsics) =====
    {"memcpy", SID_MEMCPY},
    {"memmove", SID_MEMCPY},
    {"__memcpy_chk", SID_MEMCPY},
    {"__memmove_chk", SID_MEMCPY},
    {"wmemcpy", SID_MEMCPY},
    {"wmemmove", SID_MEMCPY},
    {"llvm_memcpy", SID_MEMCPY},
    {"llvm_memcpy_p0i8_p0i8_i64", SID_MEMCPY},
    {"llvm_memcpy_p0_p0_i64", SID_MEMCPY},
    {"llvm_memcpy_p0i8_p0i8_i32", SID_MEMCPY},
    {"llvm_memmove", SID_MEMCPY},
    {"llvm_memmove_p0i8_p0i8_i64", SID_MEMCPY},
    {"llvm_memmove_p0_p0_i64", SID_MEMCPY},
    {"llvm_memmove_p0i8_p0i8_i32", SID_MEMCPY},

    /// ===== SID_BCOPY =====
    {"bcopy", SID_BCOPY},

    /// ===== SID_MEMCPY_S (C11 Annex K / securec) =====
    {"memcpy_s", SID_MEMCPY_S},
    {"memmove_s", SID_MEMCPY_S},

    /// ===== SID_MEMSET =====
    {"memset", SID_MEMSET},
    {"__memset_chk", SID_MEMSET},
    {"wmemset", SID_MEMSET},
    {"llvm_memset", SID_MEMSET},
    {"llvm_memset_p0i8_i32", SID_MEMSET},
    {"llvm_memset_p0i8_i64", SID_MEMSET},
    {"llvm_memset_p0_i32", SID_MEMSET},
    {"llvm_memset_p0_i64", SID_MEMSET},

    /// ===== SID_MEMSET_S =====
    {"memset_s", SID_MEMSET_S},

    /// ===== SID_BZERO =====
    {"bzero", SID_BZERO},

    /// ===== SID_STRN_EXPLICIT =====
    {"strncpy", SID_STRN_EXPLICIT},
    {"__strncpy_chk", SID_STRN_EXPLICIT},
    {"strncat", SID_STRN_EXPLICIT},
    {"__strncat_chk", SID_STRN_EXPLICIT},
    {"strncpy_s", SID_STRN_EXPLICIT},
    {"strncat_s", SID_STRN_EXPLICIT},

    /// ===== SID_STR_IMPLICIT =====
    {"strcpy", SID_STR_IMPLICIT},
    {"__strcpy_chk", SID_STR_IMPLICIT},
    {"stpcpy", SID_STR_IMPLICIT},

    /// ===== SID_STRCAT_IMPLICIT =====
    {"strcat", SID_STRCAT_IMPLICIT},
    {"__strcat_chk", SID_STRCAT_IMPLICIT},

    /// ===== SID_SNPRINTF =====
    {"snprintf", SID_SNPRINTF},
    {"__snprintf_chk", SID_SNPRINTF},
    {"vsnprintf", SID_SNPRINTF},

    /// ===== SID_MEMCCPY =====
    {"memccpy", SID_MEMCCPY},

    /// ===== SID_USER_COPY =====
    {"copy_from_user", SID_USER_COPY},
    {"raw_copy_from_user", SID_USER_COPY},

    {nullptr, SID_MEMCPY}
};

static std::string stripMemTransferIntrinsicSuffix(std::string s)
{
    const std::string suffixes[] = {"_p0i8_p0i8_i64", "_p0_p0_i64", "_p0i8_p0i8_i32",
                                    "_p0i8_i64", "_p0i8_i32", "_p0_i64", "_p0_i32"};
    for (const std::string& suf : suffixes)
    {
        if (s.size() > suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0)
            return s.substr(0, s.size() - suf.size());
    }
    static const std::string chk = "_chk";
    if (s.size() > chk.size() && s.compare(s.size() - chk.size(), chk.size(), chk) == 0)
        return s.substr(0, s.size() - chk.size());
    return s;
}

std::string SaberMemTransferAPI::normalizeFuncName(const std::string& name)
{
    std::string s = name;
    if (s.compare(0, 6, "llvm.") == 0)
        s = s.substr(6);
    for (char& c : s)
    {
        if (c == '.')
            c = '_';
    }
    if (s.compare(0, 5, "llvm_") == 0)
        s = s.substr(5);
    return stripMemTransferIntrinsicSuffix(s);
}

void SaberMemTransferAPI::init()
{
    specTemplates.clear();
    specTemplates.reserve(SID_NUM_SPECS);
    for (u16_t i = 0; i < SID_NUM_SPECS; ++i)
        specTemplates.push_back(spec_templates[i]);

    std::set<u16_t> specSeen;
    u16_t prevSpec = SID_NUM_SPECS;
    for (const mt_ei_pair* p = mt_pairs; p->n != nullptr; ++p)
    {
        if (p->specId != prevSpec)
        {
            if (specSeen.count(p->specId))
            {
                std::fputs(p->n, stderr);
                std::fputc('\n', stderr);
                assert(!"mt_pairs not grouped by specId");
            }
            specSeen.insert(p->specId);
            prevSpec = p->specId;
        }

        if (nameToSpecMap.count(p->n))
        {
            std::fputs(p->n, stderr);
            std::fputc('\n', stderr);
            assert(!"duplicate name in mt_pairs");
        }
        nameToSpecMap[p->n] = p->specId;
    }
}

u16_t SaberMemTransferAPI::lookupSpecId(const std::string& name) const
{
    NameToSpecMap::const_iterator it = nameToSpecMap.find(name);
    if (it != nameToSpecMap.end())
        return it->second;

    const std::string norm = normalizeFuncName(name);
    if (norm != name)
    {
        it = nameToSpecMap.find(norm);
        if (it != nameToSpecMap.end())
            return it->second;
    }
    return SID_NUM_SPECS;
}

const SaberMemTransferAPI::TransferSpec* SaberMemTransferAPI::getSpecByName(const std::string& name) const
{
    if (const SaberSemanticRules::Rule* rule =
            SaberSemanticRules::get()->find(name, SaberSemanticRules::Kind::MEMORY_TRANSFER))
    {
        dynamicAccessRules.clear();
        if (rule->targetArg >= 0)
            dynamicAccessRules.push_back(
                {static_cast<s8_t>(rule->targetArg), static_cast<s8_t>(rule->lengthArg)});
        if (rule->sourceArg >= 0)
            dynamicAccessRules.push_back(
                {static_cast<s8_t>(rule->sourceArg), static_cast<s8_t>(rule->lengthArg)});
        dynamicSpec.kind = rule->effect == "set" ? TK_SET :
                           rule->effect == "string_copy" ? TK_STRING_COPY : TK_COPY;
        dynamicSpec.ptrProp = {
            static_cast<u8_t>(rule->targetArg < 0 ? 0 : rule->targetArg),
            static_cast<u8_t>(rule->sourceArg < 0 ? 0 : rule->sourceArg),
            static_cast<s8_t>(rule->lengthArg), rule->targetArg >= 0 && rule->sourceArg >= 0
        };
        dynamicSpec.accessRules = dynamicAccessRules.empty() ? nullptr : dynamicAccessRules.data();
        dynamicSpec.numAccessRules = static_cast<u8_t>(dynamicAccessRules.size());
        dynamicSpec.strCopyLike = dynamicSpec.kind == TK_STRING_COPY;
        dynamicSpec.strSrcArgIdx = static_cast<u8_t>(rule->sourceArg < 0 ? 0 : rule->sourceArg);
        return &dynamicSpec;
    }
    const u16_t id = lookupSpecId(name);
    if (id >= specTemplates.size())
        return nullptr;
    return &specTemplates[id];
}

const SaberMemTransferAPI::TransferSpec* SaberMemTransferAPI::getSpec(const FunObjVar* fun) const
{
    if (fun == nullptr)
        return nullptr;
    return getSpecByName(fun->getName());
}

bool SaberMemTransferAPI::isStrCopyLike(const std::string& funcName) const
{
    const TransferSpec* spec = getSpecByName(funcName);
    return spec && spec->strCopyLike;
}

const std::vector<MemAccessRule> SaberMemTransferAPI::getAccessRules(const std::string& funcName) const
{
    const TransferSpec* spec = getSpecByName(funcName);
    if (spec == nullptr || spec->accessRules == nullptr || spec->numAccessRules == 0)
        return {};
    return std::vector<MemAccessRule>(spec->accessRules,
                                      spec->accessRules + spec->numAccessRules);
}

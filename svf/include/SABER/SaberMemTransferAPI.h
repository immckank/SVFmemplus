//===- SaberMemTransferAPI.h -- Memory copy/set API for checkers ------------===//
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
 * SaberMemTransferAPI.h
 *
 * Shared memory-copy / memory-set / string-transfer API summary table.
 * Style and lifecycle mirror SaberCheckerAPI (static ei_pairs, init-once map).
 */

#ifndef SABERMEMTRANSFERAPI_H_
#define SABERMEMTRANSFERAPI_H_

#include "Util/SVFUtil.h"
#include "Graphs/ICFGNode.h"
#include "SVFIR/SVFVariables.h"
#include <string>
#include <vector>

namespace SVF
{

/**
 * @brief Buffer access rule: argument @p bufArgIdx is accessed for @p lenArgIdx bytes.
 * @p lenArgIdx == -1 means implicit length (e.g. strcpy => strlen(src)).
 */
struct MemAccessRule
{
    s8_t bufArgIdx;
    s8_t lenArgIdx;
};

/**
 * @brief Function-level memory transfer summary (template).
 */
class SaberMemTransferAPI
{
public:
    enum TRANSFER_KIND
    {
        TK_DUMMY = 0,
        TK_COPY,          /// memcpy / memmove / llvm.memcpy
        TK_SET,           /// memset / bzero
        TK_STRING_COPY,   /// strcpy / strncpy / *_s variants
        TK_STRING_CAT,    /// strcat / strncat
        TK_FORMAT_WRITE,  /// snprintf / vsnprintf
        TK_USER_COPY,     /// copy_from_user (Uninit treats differently)
    };

    struct PtrPropagateSpec
    {
        u8_t dstArgIdx;
        u8_t srcArgIdx;
        s8_t lenArgIdx;
        bool enabled;
    };

    struct TransferSpec
    {
        TRANSFER_KIND kind;
        PtrPropagateSpec ptrProp;
        const MemAccessRule* accessRules;
        u8_t numAccessRules;
        bool strCopyLike;   /// implicit length from source string
        u8_t strSrcArgIdx;  /// valid when strCopyLike
    };

    typedef Map<std::string, u16_t> NameToSpecMap;

private:
    NameToSpecMap nameToSpecMap;
    std::vector<TransferSpec> specTemplates;
    mutable std::vector<MemAccessRule> dynamicAccessRules;
    mutable TransferSpec dynamicSpec{};

    SaberMemTransferAPI()
    {
        init();
    }

    void init();

    static SaberMemTransferAPI* mtAPI;

    u16_t lookupSpecId(const std::string& name) const;

public:
    static SaberMemTransferAPI* getAPI()
    {
        if (mtAPI == nullptr)
            mtAPI = new SaberMemTransferAPI();
        return mtAPI;
    }

    const TransferSpec* getSpec(const FunObjVar* fun) const;
    const TransferSpec* getSpecByName(const std::string& name) const;

    inline bool isMemTransfer(const FunObjVar* fun) const
    {
        return getSpec(fun) != nullptr;
    }

    inline bool isMemTransfer(const CallICFGNode* cs) const
    {
        return cs && isMemTransfer(cs->getCalledFunction());
    }

    inline bool isMemcpyLike(const FunObjVar* fun) const
    {
        const TransferSpec* spec = getSpec(fun);
        return spec && (spec->kind == TK_COPY || spec->kind == TK_STRING_COPY ||
                        spec->kind == TK_STRING_CAT);
    }

    inline bool isMemsetLike(const FunObjVar* fun) const
    {
        const TransferSpec* spec = getSpec(fun);
        return spec && spec->kind == TK_SET;
    }

    inline bool isUserCopyLike(const FunObjVar* fun) const
    {
        const TransferSpec* spec = getSpec(fun);
        return spec && spec->kind == TK_USER_COPY;
    }

    bool isStrCopyLike(const std::string& funcName) const;

    const std::vector<MemAccessRule> getAccessRules(const std::string& funcName) const;

    static std::string normalizeFuncName(const std::string& name);
};

} // namespace SVF

#endif /* SABERMEMTRANSFERAPI_H_ */

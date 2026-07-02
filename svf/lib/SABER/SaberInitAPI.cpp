//===- SaberInitAPI.cpp -- Queryable initializer-function table for Saber ---//
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
 * SaberInitAPI.cpp
 */

#include "SABER/SaberInitAPI.h"
#include "SABER/SaberSemanticRules.h"

using namespace SVF;

SaberInitAPI* SaberInitAPI::initAPI = nullptr;

/*!
 * The initializer-function table. SEED ONLY with functions that fully and
 * unconditionally initialize the named argument's pointee (listing a partial
 * initializer risks a false negative). memset/memcpy/bzero are intentionally
 * NOT duplicated here — they are handled by SaberMemTransferAPI.
 *
 * This list is meant to be extended offline (e.g. an LLM classifies project
 * functions from their signature/body). Keep each entry's `reason` accurate so
 * the table stays auditable via -saber-scope-dump.
 */
void SaberInitAPI::init()
{
    static const InitRule kRules[] =
    {
        // Example/common initializers. Project-specific entries are expected to be
        // appended offline. Each must FULLY initialize the referenced argument.
        {"explicit_bzero",      true, 0, "libc explicit_bzero(ptr,n): zero-initializes *arg0"},
        {"__explicit_bzero_chk",true, 0, "fortified explicit_bzero: zero-initializes *arg0"},
        {"__memset_chk",        true, 0, "fortified memset: initializes *arg0"},
        {"memzero_explicit",    true, 0, "libc memzero_explicit(ptr,n): zero-initializes *arg0"},
        {"bzero",               true, 0, "libc bzero(ptr,n): zero-initializes *arg0"},
        {"__bzero",             true, 0, "libc __bzero(ptr,n): zero-initializes *arg0"},
        {"memset",              true, 0, "libc memset(ptr,c,n) with c==0: zero-initializes *arg0"},
        {"wmemset",             true, 0, "libc wmemset(ptr,c,n) with c==0: zero-initializes *arg0"},
        // Huawei / openEuler SecureC (FalconFS): memset_s(dest,destMax,c,count) zeroes *arg0
        {"memset_s",            true, 0, "SecureC memset_s(dest,destMax,c,count): zero-initializes *arg0"},
        {"wmemset_s",           true, 0, "SecureC wmemset_s(dest,destMax,c,count): zero-initializes *arg0"},
        // LLVM memset intrinsics (opaque call sites without a modeled store edge)
        {"llvm_memset",             true, 0, "LLVM memset intrinsic: zero-initializes *arg0"},
        {"llvm_memset_p0i8_i32",    true, 0, "LLVM memset intrinsic: zero-initializes *arg0"},
        {"llvm_memset_p0i8_i64",    true, 0, "LLVM memset intrinsic: zero-initializes *arg0"},
        {"llvm_memset_p0_i32",      true, 0, "LLVM memset intrinsic: zero-initializes *arg0"},
        {"llvm_memset_p0_i64",      true, 0, "LLVM memset intrinsic: zero-initializes *arg0"},
        // --- Append LLM-classified project initializers below this line ---
    };

    rules.assign(kRules, kRules + sizeof(kRules) / sizeof(kRules[0]));
}

bool SaberInitAPI::isInitializerName(const std::string& name,
                                     int* outArgIdx,
                                     const char** outReason) const
{
    if (name.empty())
        return false;
    if (const SaberSemanticRules::Rule* rule =
            SaberSemanticRules::get()->find(name, SaberSemanticRules::Kind::INITIALIZER))
    {
        if (outArgIdx) *outArgIdx = rule->targetArg;
        if (outReason) *outReason = rule->reason.c_str();
        return true;
    }
    for (const InitRule& r : rules)
    {
        const bool hit = r.exactMatch ? (name == r.name)
                         : (name.find(r.name) != std::string::npos);
        if (hit)
        {
            if (outArgIdx) *outArgIdx = r.argIdx;
            if (outReason) *outReason = r.reason;
            return true;
        }
    }
    return false;
}

bool SaberInitAPI::isInitializer(const FunObjVar* fun,
                                 int* outArgIdx,
                                 const char** outReason) const
{
    if (fun == nullptr)
        return false;
    return isInitializerName(fun->getName(), outArgIdx, outReason);
}

void SaberInitAPI::dump(std::ostream& os) const
{
    os << "===== SaberInitAPI: registered initializer functions =====\n";
    os << "(a call to any function below kills the uninitialized region of the named arg)\n";
    if (rules.empty())
        os << "  <empty -- populate offline, e.g. via LLM classification>\n";
    for (const InitRule& r : rules)
    {
        os << "  name=\"" << r.name << "\""
           << " match=" << (r.exactMatch ? "exact" : "substr")
           << " arg=" << (r.argIdx == ANY_ARG ? std::string("any") : std::to_string(r.argIdx))
           << " :: " << r.reason << "\n";
    }
    os << "==========================================================\n";
    os.flush();
}

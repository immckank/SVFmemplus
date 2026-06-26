//===- SaberScopeAPI.cpp -- Queryable analysis-scope table for Saber --------//
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
 * SaberScopeAPI.cpp
 */

#include "SABER/SaberScopeAPI.h"

using namespace SVF;

SaberScopeAPI* SaberScopeAPI::scopeAPI = nullptr;

/*!
 * The scope-exclusion rule table. Each entry documents *why* code matching the
 * pattern is excluded from Saber source/sink collection. Keep this list as the
 * single source of truth for scope decisions; do not add ad-hoc path checks in
 * the detectors.
 */
void SaberScopeAPI::init()
{
    static const ScopeRule kRules[] =
    {
        // --- C++ standard library / toolchain headers (by source-location path) ---
        {"/include/c++/",      SM_PATH, "stdlib",    "libstdc++ standard-library header; STL internals are not application code"},
        {"/usr/lib/gcc/",      SM_PATH, "system",    "GCC toolchain / libstdc++ install path"},
        {"/usr/local/include/",SM_PATH, "system",    "system-wide third-party include directory"},
        {"/usr/include/",      SM_PATH, "system",    "system C/C++ headers (libc, system libraries)"},

        // --- machine-generated code (by source-location path) ---
        {".pb.cc",             SM_PATH, "generated", "protobuf-generated C++ translation unit"},
        {"_generated.h",       SM_PATH, "generated", "generated header (by-convention *_generated.h)"},
        {"/google/protobuf/",  SM_PATH, "generated", "bundled protobuf runtime headers"},
        {"/protobuf/",         SM_PATH, "generated", "protobuf runtime / generated bindings"},

        // --- C++ standard library / ABI (by Itanium-mangled function name) ---
        // Catches STL functions whose defining object carries no usable file
        // location but whose mangled name encodes the std:: / __gnu_cxx namespace.
        {"_ZNSt",   SM_FUNC_MANGLED, "stdlib", "member function in namespace std (Itanium mangling _ZNSt...)"},
        {"_ZNKSt",  SM_FUNC_MANGLED, "stdlib", "const member function in namespace std (_ZNKSt...)"},
        {"_ZSt",    SM_FUNC_MANGLED, "stdlib", "free function in namespace std (_ZSt...)"},
        {"_ZN9__gnu_cxx", SM_FUNC_MANGLED, "stdlib", "function in namespace __gnu_cxx (libstdc++ extensions)"},
        {"_ZNK9__gnu_cxx",SM_FUNC_MANGLED, "stdlib", "const function in namespace __gnu_cxx"},
    };

    rules.assign(kRules, kRules + sizeof(kRules) / sizeof(kRules[0]));
}

bool SaberScopeAPI::isOutOfScopePath(const std::string& rawLoc,
                                     const char** outReason,
                                     const char** outCategory) const
{
    if (rawLoc.empty())
        return false;
    for (const ScopeRule& r : rules)
    {
        if (r.kind != SM_PATH)
            continue;
        if (rawLoc.find(r.pattern) != std::string::npos)
        {
            if (outReason) *outReason = r.reason;
            if (outCategory) *outCategory = r.category;
            return true;
        }
    }
    return false;
}

bool SaberScopeAPI::isOutOfScopeFunctionName(const std::string& mangledName,
                                             const char** outReason,
                                             const char** outCategory) const
{
    if (mangledName.empty())
        return false;
    for (const ScopeRule& r : rules)
    {
        if (r.kind != SM_FUNC_MANGLED)
            continue;
        if (mangledName.find(r.pattern) != std::string::npos)
        {
            if (outReason) *outReason = r.reason;
            if (outCategory) *outCategory = r.category;
            return true;
        }
    }
    return false;
}

bool SaberScopeAPI::isOutOfScopeFunction(const FunObjVar* fun,
                                         const char** outReason,
                                         const char** outCategory) const
{
    if (fun == nullptr)
        return false;
    if (isOutOfScopePath(fun->getSourceLoc(), outReason, outCategory))
        return true;
    return isOutOfScopeFunctionName(fun->getName(), outReason, outCategory);
}

void SaberScopeAPI::dump(std::ostream& os) const
{
    os << "===== SaberScopeAPI: analysis-scope exclusion rules =====\n";
    os << "(code matching any rule below is excluded from Saber source/sink collection)\n";
    for (const ScopeRule& r : rules)
    {
        os << "  [" << (r.kind == SM_PATH ? "path" : "func") << "]"
           << " category=" << r.category
           << " pattern=\"" << r.pattern << "\""
           << " :: " << r.reason << "\n";
    }
    os << "=========================================================\n";
    os.flush();
}

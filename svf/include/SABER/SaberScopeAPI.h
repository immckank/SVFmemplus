//===- SaberScopeAPI.h -- Queryable analysis-scope table for Saber ----------//
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
 * SaberScopeAPI.h
 *
 * An explicit, queryable, and self-documenting table of *analysis-scope*
 * exclusion rules for the Saber checkers — the scope counterpart of
 * SaberCheckerAPI (which lists alloc/free/file APIs).
 *
 * Motivation: checkers such as UninitChecker waste time and emit noise on
 * sources/sinks that originate in system, C++ standard-library, or
 * machine-generated code. Rather than burying ad-hoc path-substring checks
 * inside detector logic (which are easy to get wrong and impossible to audit —
 * e.g. a JSON-parse that silently fails on prefixed location strings), every
 * scope-exclusion rule lives here as a named entry carrying a human-readable
 * reason, and can be dumped via `-saber-scope-dump`.
 *
 * Matching is a raw substring test (no JSON parsing), so it works on both the
 * pure-JSON source locations of value-flow nodes and the type-prefixed
 * locations of ICFG call nodes (e.g. "CallICFGNode: { ... }").
 */

#ifndef SABERSCOPEAPI_H_
#define SABERSCOPEAPI_H_

#include "Util/SVFUtil.h"
#include "SVFIR/SVFVariables.h"
#include <string>
#include <vector>
#include <ostream>

namespace SVF
{

class SaberScopeAPI
{
public:
    /// What a rule's pattern is matched against.
    enum ScopeMatchKind
    {
        SM_PATH,         ///< matched against a source-location string (file path)
        SM_FUNC_MANGLED  ///< matched against a function's (mangled) name
    };

    struct ScopeRule
    {
        const char* pattern;     ///< substring that triggers the rule
        ScopeMatchKind kind;     ///< how `pattern` is matched
        const char* category;    ///< short bucket, e.g. "stdlib", "system", "generated"
        const char* reason;      ///< human-readable justification for exclusion
    };

private:
    std::vector<ScopeRule> rules;
    static SaberScopeAPI* scopeAPI;

    SaberScopeAPI()
    {
        init();
    }
    void init();

public:
    static SaberScopeAPI* getScopeAPI()
    {
        if (scopeAPI == nullptr)
            scopeAPI = new SaberScopeAPI();
        return scopeAPI;
    }

    /// The full rule table (for inspection / dumping).
    const std::vector<ScopeRule>& getRules() const
    {
        return rules;
    }

    /// Whether a raw source-location string is out of scope by some SM_PATH rule.
    /// If `outReason`/`outCategory` are non-null, the matching rule is reported.
    bool isOutOfScopePath(const std::string& rawLoc,
                          const char** outReason = nullptr,
                          const char** outCategory = nullptr) const;

    /// Whether a (mangled) function name is out of scope by some SM_FUNC_MANGLED rule.
    bool isOutOfScopeFunctionName(const std::string& mangledName,
                                  const char** outReason = nullptr,
                                  const char** outCategory = nullptr) const;

    /// Whether a function is out of scope, checking both its source location
    /// (SM_PATH) and its mangled name (SM_FUNC_MANGLED).
    bool isOutOfScopeFunction(const FunObjVar* fun,
                              const char** outReason = nullptr,
                              const char** outCategory = nullptr) const;

    /// Print the rule table (used by `-saber-scope-dump`).
    void dump(std::ostream& os) const;
};

} // End namespace SVF

#endif /* SABERSCOPEAPI_H_ */

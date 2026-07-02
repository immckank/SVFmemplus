//===- SaberInitAPI.h -- Queryable initializer-function table for Saber -----//
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
 * SaberInitAPI.h
 *
 * An explicit, queryable, self-documenting table of *initializer* functions for
 * the uninitialized-use checker — the initialization counterpart of
 * SaberCheckerAPI (alloc/free) and SaberMemTransferAPI (memset/memcpy).
 *
 * Motivation: the hardest part of uninitialized-use detection is modelling *what
 * counts as initialization*. SaberMemTransferAPI only knows memset/memcpy/
 * copy_from_user; any other function that writes a caller-supplied buffer (a
 * project's `reset()`, a custom `xxx_init(&buf)`, a deserialiser, etc.) is
 * invisible, so a later read of that buffer is reported as a (false) uninit use.
 *
 * This table is the place to declare such functions. It is intended to be
 * populated *offline* — e.g. by an LLM that reads a function's signature/body
 * and classifies "does this initialize pointer argument N?" — with every entry
 * human-reviewable and dumpable via `-saber-scope-dump`. The static analyzer
 * then consumes the table deterministically and soundly: a call to a registered
 * initializer kills the uninitialized region of the argument it initializes.
 *
 * Only register functions that *fully and unconditionally* initialize the named
 * argument's pointee; partial/conditional initializers must NOT be listed, as
 * that could suppress a real bug (false negative).
 */

#ifndef SABERINITAPI_H_
#define SABERINITAPI_H_

#include "Util/SVFUtil.h"
#include "SVFIR/SVFVariables.h"
#include "Graphs/ICFGNode.h"
#include <string>
#include <vector>
#include <ostream>

namespace SVF
{

class SaberInitAPI
{
public:
    /// Argument index sentinel meaning "any pointer argument".
    static constexpr int ANY_ARG = -1;

    struct InitRule
    {
        const char* name;     ///< exact function name (or unique substring) to match
        bool exactMatch;      ///< true: equality; false: substring match on the name
        int  argIdx;          ///< which pointer arg is initialized (ANY_ARG = any)
        const char* reason;   ///< human-readable justification
    };

private:
    std::vector<InitRule> rules;
    static SaberInitAPI* initAPI;

    SaberInitAPI()
    {
        init();
    }
    void init();

public:
    static SaberInitAPI* getInitAPI()
    {
        if (initAPI == nullptr)
            initAPI = new SaberInitAPI();
        return initAPI;
    }

    const std::vector<InitRule>& getRules() const
    {
        return rules;
    }

    /// Whether a (possibly mangled) function name is a registered initializer.
    bool isInitializerName(const std::string& name,
                           int* outArgIdx = nullptr,
                           const char** outReason = nullptr) const;

    /// Whether a function is a registered initializer.
    bool isInitializer(const FunObjVar* fun,
                       int* outArgIdx = nullptr,
                       const char** outReason = nullptr) const;

    /// Print the rule table (used by `-saber-scope-dump`).
    void dump(std::ostream& os) const;
};

} // End namespace SVF

#endif /* SABERINITAPI_H_ */

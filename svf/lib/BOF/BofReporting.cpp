//===- BofReporting.cpp -- Buffer-overflow report dedup & emission --------===//
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
 *  BofReporting.cpp
 *
 *  Reporting half of BufferOverflowChecker: record-time and cross-context
 *  deduplication, structured SVFBugReport emission, human-readable terminal
 *  output, and the LLM MAY-triage tail. Split out of BufferOverflowChecker.cpp
 *  (the class declaration / state lives in BOF/BufferOverflowChecker.h; this is
 *  purely an additional translation unit).
 *
 *  Created on: Nov 10 , 2025
 *      Author: Yaokun Yang
 */

#include "BOF/BufferOverflowChecker.h"
#include "Graphs/ICFGNode.h"
#include "Util/SVFUtil.h"

using namespace SVF;

namespace
{
/// Whether a buffer-overflow report at source-location string @p locStr is
/// *out of scope* and should be suppressed because it does not belong to the
/// analyzed application's own (first-party) source.
///
/// Rationale (false-positive reduction). On a real C++ codebase the vast
/// majority of raw BOF candidates are not defects in the program under analysis
/// but modeling artifacts of code that is merely *included* into it:
///   * System / C++ standard-library headers (<format>, <optional>, libstdc++
///     <bits/...>): SVF models their internal fixed-size buffers (SSO storage,
///     std::variant/optional union storage, std::format scratch arrays) as
///     plain arrays and then flags the library's own bounds-juggling as
///     overflow. These are never actionable by the application developer.
///   * Compiler-/codegen-generated serialization stubs (protobuf *.pb.cc /
///     *.pb.h, flatbuffers *_generated.h): machine-generated accessor code whose
///     pointer arithmetic SVF cannot relate to the real wire-format invariants.
///   * Locations stripped of debug info (no "file" field): typically
///     pointer-storage allocas / thunks that cannot be tied back to any user
///     source line, so a report on them is unactionable noise.
///
/// This mirrors the system-header / generated-code suppression that production
/// static analyzers (clang-tidy's --system-headers=0, Coverity) apply by
/// default. It is a *display/emission* filter only: the sound MUST/MAY
/// classification of the surviving (first-party) reports is unchanged.
bool isOutOfScopeReport(const std::string& locStr)
{
    // Pull the file value out of the JSON-ish source-loc string. NB: the *raw*
    // string returned by getSourceLoc() uses SVF core's terse key "fl" (only the
    // terminal pretty-printer rewrites it to "file" via friendlyLoc), e.g.
    //   CallICFGNode: { "ln": 98, "cl": 9, "fl": "falcon_client/..." }
    static const std::string kFileKey = "\"fl\":";
    const size_t kp = locStr.find(kFileKey);
    if (kp == std::string::npos)
        return true; // no debug location -> not attributable to user source
    const size_t q1 = locStr.find('"', kp + kFileKey.size());
    if (q1 == std::string::npos)
        return true;
    const size_t q2 = locStr.find('"', q1 + 1);
    if (q2 == std::string::npos)
        return true;
    const std::string file = locStr.substr(q1 + 1, q2 - q1 - 1);
    if (file.empty())
        return true;

    // System / third-party / standard-library path markers.
    static const char* kDenySubstr[] = {
        "/usr/",         // system + libstdc++ install prefix
        "/include/c++/", // libstdc++ headers
        "/lib/gcc/",     // gcc internal headers
        "/bits/",        // libstdc++ detail headers
        "/generated/",   // generated-code trees
    };
    for (const char* m : kDenySubstr)
        if (file.find(m) != std::string::npos)
            return true;

    // Machine-generated serialization stubs (match on filename suffix).
    static const char* kDenySuffix[] = {".pb.cc", ".pb.h", "_generated.h"};
    for (const char* suf : kDenySuffix)
    {
        const std::string s = suf;
        if (file.size() >= s.size() &&
            file.compare(file.size() - s.size(), s.size(), s) == 0)
            return true;
    }
    // A leading "generated/" prefix (relative path) is generated code too.
    if (file.compare(0, 10, "generated/") == 0)
        return true;

    return false;
}

/// Make source-location strings friendlier for end users.
///
/// SVF core (svf-llvm/LLVMUtil.cpp::getSourceLoc) serialises locations with the
/// terse keys "ln" / "cl" / "fl". We must not touch that shared code, so the
/// BOF module post-processes the strings it is about to print, rewriting the
/// keys to their full names: "ln"->"line", "cl"->"col", "fl"->"file".
///
/// Replacement is anchored on the exact quoted-key-plus-colon token (e.g.
/// `"ln":`) so it never matches substrings inside file paths or value names.
std::string friendlyLoc(std::string s)
{
    static const std::pair<const char*, const char*> kRewrites[] = {
        {"\"ln\":", "\"line\":"},
        {"\"cl\":", "\"col\":"},
        {"\"fl\":", "\"file\":"},
    };
    for (const auto& r : kRewrites)
    {
        const std::string from = r.first;
        const std::string to = r.second;
        for (size_t pos = s.find(from); pos != std::string::npos;
             pos = s.find(from, pos + to.size()))
        {
            s.replace(pos, from.size(), to);
        }
    }
    return s;
}

const char* bofKindStr(BofKind kind)
{
    switch (kind)
    {
    case BofKind::GEP_OOB:    return "array/pointer access";
    case BofKind::MEMCPY_OOB: return "memcpy/memmove";
    case BofKind::MEMSET_OOB: return "memset";
    case BofKind::STRCPY_OOB: return "strcpy/strcat";
    }
    return "access";
}
} // namespace

void BufferOverflowChecker::reportBufferOverflowError(const SVFVar* base, const Range& offset,
                                                      const Range& size, bool isHeap,
                                                      BofKind kind, const ICFGNode* loc,
                                                      bool mustOverflow,
                                                      const ICFGNode* callContext,
                                                      const SVFVar* indexVar)
{
    // Record-time dedup by (source location, kind, call context). The call
    // context (k=1 call-site, null for intraprocedural accesses) keeps the
    // *same* callee instruction reached under *different* contexts distinct
    // (e.g. write_at(a,11) and write_at(a,10) both surface at `p[idx]`), so
    // each per-context verdict is captured once. Final cross-context dedup
    // (suppressing a MAY when the same access point also yields a MUST) is
    // performed later in flushReports().
    // Scope filter (false-positive reduction): drop candidates that fall in
    // system / standard-library headers, machine-generated serialization stubs,
    // or debug-info-less locations. These are dominated by modeling artifacts of
    // transitively-included library internals rather than defects in the program
    // under analysis, and are not actionable by the application developer.
    if (!loc || isOutOfScopeReport(loc->getSourceLoc()))
        return;

    if (loc)
    {
        std::string key = loc->getSourceLoc() + "#" + std::to_string((int)kind);
        if (callContext)
            key += "@" + std::to_string(callContext->getId());
        if (!bugLoc.insert(key).second)
            return;
    }

    PendingReport pr;
    pr.base = base;
    pr.offset = offset;
    pr.size = size;
    pr.isHeap = isHeap;
    pr.kind = kind;
    pr.loc = loc;
    pr.mustOverflow = mustOverflow;
    pr.callContext = callContext;
    pr.indexVar = indexVar;
    pendingReports.push_back(pr);
}

void BufferOverflowChecker::flushReports()
{
    // First pass: collect every access point (source location + kind) at which
    // some calling context proves a MUST overflow.
    std::set<std::string> mustPoints;
    for (const PendingReport& pr : pendingReports)
    {
        if (pr.mustOverflow && pr.loc)
            mustPoints.insert(pr.loc->getSourceLoc() + "#" + std::to_string((int)pr.kind));
    }

    // Second pass: emit, with two layers of dedup:
    //  (a) suppress a MAY at any access point that also has a MUST (avoid
    //      double-reporting the same point, e.g. the null-context MAY alongside
    //      the call-site-context MUST -- keep the higher-information MUST);
    //  (b) collapse exact-duplicate verdicts (same loc/kind/severity and same
    //      offset interval) arising from seeding one buffer under several call
    //      contexts that all compute the *same* index. Genuinely distinct
    //      per-context verdicts (different offset intervals, e.g. write_at(a,11)
    //      vs write_at(a,10)) are preserved.
    std::set<std::string> emitted;
    // Surviving loop-induction MAYs eligible for LLM triage: slice id -> the
    // friendly source location (for the terminal annotation pass below).
    std::vector<std::pair<std::string, std::string>> triagedMays;
    for (const PendingReport& pr : pendingReports)
    {
        if (pr.loc)
        {
            const std::string point =
                pr.loc->getSourceLoc() + "#" + std::to_string((int)pr.kind);
            if (!pr.mustOverflow && mustPoints.count(point))
                continue; // suppressed in favour of the MUST at this point

            // Content key: same point + severity + offset interval => duplicate.
            const std::string contentKey =
                point + (pr.mustOverflow ? "!MUST" : "!MAY") +
                "[" + std::to_string(pr.offset.getLower()) + "," +
                std::to_string(pr.offset.getUpper()) + "]";
            if (!emitted.insert(contentKey).second)
                continue;
        }

        // Presentation-only severity: the "client-special" switch renders a MAY
        // as MUST. Applied strictly after the dedup above (which keys on the
        // real pr.mustOverflow), so the set of emitted points is identical to
        // the default run -- only the displayed label / bug kind changes.
        const bool showMust = pr.mustOverflow || mayAsMust;

        // Structured report (requires a non-empty event stack).
        if (pr.loc)
        {
            GenericBug::EventStack eventStack;
            eventStack.push_back(SVFBugEvent(SVFBugEvent::SourceInst, pr.loc));
            bugReport.addAbsExecBug(
                showMust ? GenericBug::FULLBUFOVERFLOW : GenericBug::PARTIALBUFOVERFLOW,
                eventStack, pr.size.getLower(), pr.size.getUpper(),
                pr.offset.getLower(), pr.offset.getUpper());
        }

        // Human-readable terminal output.
        SVFUtil::outs() << "[BufferOverflowChecker] "
                        << (showMust ? "MUST" : "MAY") << " buffer overflow ("
                        << bofKindStr(pr.kind) << ")\n"
                        << "  Base       : " << friendlyLoc(pr.base->toString()) << "\n"
                        << "  Access     : " << pr.offset.toString() << "\n"
                        << "  Valid range: " << pr.size.toString()
                        << (pr.isHeap ? "  (bytes)\n" : "  (elements)\n");
        if (pr.loc)
            SVFUtil::outs() << "  Location   : " << friendlyLoc(pr.loc->getSourceLoc()) << "\n";
        SVFUtil::outs() << "\n";

        // LLM MAY-triage overlay (pure add-on): slice every surviving GEP_OOB
        // MAY carrying a symbolic index (loop-induction or guarded). After the
        // guard/loop narrowing such an index may now be a finite interval rather
        // than TOP, so we no longer gate on unboundedness. Read-only over the
        // IR; the sound report emitted above is untouched.
        if (!pr.mustOverflow && pr.kind == BofKind::GEP_OOB &&
            pr.indexVar && pr.loc)
        {
            BofSlice slice;
            if (llmTriage.collectSlice(pr.base, pr.indexVar, pr.size, pr.isHeap,
                                       pr.loc, rangeAnalysis, slice))
            {
                llmTriage.addSlice(slice);
                triagedMays.emplace_back(slice.id, friendlyLoc(pr.loc->getSourceLoc()));
            }
        }
    }

    // ===== LLM MAY-triage tail: always export slices; optionally annotate. ====
    if (!llmTriage.empty())
    {
        if (llmTriage.writeSlices())
            SVFUtil::outs() << "[LLMTriage] exported " << llmTriage.size()
                            << " slice(s) to " << llmTriage.config().sliceOutPath
                            << "\n";

        std::map<std::string, LLMVerdict> verdicts;
        if (llmTriage.config().hasApi() && llmTriage.runSidecarAndLoad(verdicts))
        {
            const double thr = llmTriage.config().threshold;
            for (const auto& tm : triagedMays)
            {
                auto it = verdicts.find(tm.first);
                if (it == verdicts.end())
                    continue;
                const LLMVerdict& v = it->second;
                // SOUNDNESS: only *upgrade* MAY -> suspected overflow. Never
                // clear to SAFE; SAFE merely lowers display priority.
                if (v.verdict == "OUT_OF_BOUNDS" && v.confidence >= thr)
                {
                    SVFUtil::outs() << "[LLMTriage] LLM_SUSPECT (overflow, conf="
                                    << v.confidence << ") at " << tm.second
                                    << "\n    max_index_reasoned: " << v.maxIndexReasoned
                                    << "\n    rationale: " << v.rationale << "\n\n";
                }
                else if (v.verdict == "SAFE")
                {
                    SVFUtil::outs() << "[LLMTriage] LLM hint SAFE (display only, MAY "
                                       "retained, conf=" << v.confidence << ") at "
                                    << tm.second << "\n\n";
                }
            }
        }
        else if (!llmTriage.config().hasApi())
        {
            SVFUtil::outs() << "[LLMTriage] no LLM endpoint configured; slices "
                               "exported for manual review only.\n";
        }
    }
}

void BufferOverflowChecker::dumpReport(const std::string& filePath)
{
    bugReport.dumpToJsonFile(filePath);
}

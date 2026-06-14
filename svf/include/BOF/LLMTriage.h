//===- LLMTriage.h -- LLM-assisted MAY-triage overlay for BOF -------------===//
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
 * LLMTriage.h
 *
 *  Created on: JUN 12, 2026
 *      Author: Yaokun Yang
 *
 * A *pure overlay* on top of the sound buffer-overflow interval analysis.
 *
 * The sound checker conservatively reports loop-induction array accesses whose
 * index degrades to TOP (e.g. `int a[10]; for (i = 0; i <= 10; i++) a[i]`) as
 * MAY overflows. This module extracts a minimal, self-contained *code slice*
 * for every such surviving MAY (access point, buffer capacity, induction
 * variable, loop guards, source snippet), serialises it as `bof-slice/v1`
 * JSON, and -- when an LLM endpoint is configured -- shells out to a Python
 * sidecar that returns a verdict per slice.
 *
 * SOUNDNESS CONTRACT: the LLM may only *upgrade* a MAY to a "suspected
 * overflow" annotation (LLM_SUSPECT) or *keep* it MAY. It can NEVER clear a
 * MAY to SAFE. The sound SVFBugReport classification is left untouched; the
 * triage result is an independent overlay (terminal annotation + JSON files).
 */

#ifndef LLM_TRIAGE_H_
#define LLM_TRIAGE_H_

#include "BOF/Range.h"

#include <map>
#include <string>
#include <vector>

namespace SVF {

class SVFVar;
class ICFGNode;
class RangeAnalysis;

/// A range encoded for the slice schema. `+INF`/`-INF` are serialised as
/// strings to avoid embedding non-finite numbers in JSON.
struct SliceRange {
    std::string lower = "0";
    std::string upper = "0";
    bool isTop = false;
    bool isBottom = false;

    /// Build from an interval-analysis Range.
    static SliceRange from(const Range& r);
};

/// A loop guard that controls the induction variable (e.g. `i <= 10`).
struct GuardInfo {
    std::string predicate;       ///< "<", "<=", ">", ">=", "==", "!=" (or raw)
    std::string lhs;             ///< index-side expression / symbol
    std::string rhs;             ///< bound-side expression / symbol / constant
    SliceRange  rhsRange;        ///< static range of the bound operand
    std::string enterLoopWhen = "unknown"; ///< "true"/"false"/"unknown"
    bool        valid = false;   ///< whether this guard was confidently associated
};

/// Best-effort recovered induction-variable shape.
struct InductionInfo {
    std::string var = "unknown";       ///< induction variable name
    std::string init = "unknown";      ///< initial value (or "unknown")
    std::string step = "unknown";      ///< per-iteration step magnitude
    std::string updateOp = "unknown";  ///< "+", "-" (or "unknown")
};

/// One extracted slice for a surviving MAY out-of-bounds access.
struct BofSlice {
    std::string id;                ///< stable id: "<kind>@<file>:<line>:<col>"
    std::string kind = "GEP_OOB";  ///< access kind
    std::string staticVerdict = "MAY";

    // ---- access point ----
    std::string file;
    int line = 0;
    int col = 0;
    std::string base;              ///< buffer base (friendly value name)
    std::string indexExpr = "unknown"; ///< symbolic affine form of the index
    SliceRange  indexRange;        ///< static range of the index (typically TOP)

    // ---- buffer ----
    SliceRange  capacity;          ///< valid index/byte range of the buffer
    bool        isHeap = false;
    std::string domain = "elements"; ///< "elements" | "bytes"

    // ---- loop structure ----
    InductionInfo induction;
    std::vector<GuardInfo> guards;

    // ---- raw context ----
    std::string codeSnippet;       ///< source lines around the access

    /// Serialise this slice to a JSON object string (no trailing newline).
    std::string toJson(const std::string& indent) const;
};

/// LLM endpoint / sidecar configuration, loaded from `-llm-config=<file>` (JSON)
/// with fallback to environment variables BOF_LLM_API_URL / BOF_LLM_API_KEY /
/// BOF_LLM_MODEL.
struct LLMTriageConfig {
    std::string apiUrl;
    std::string apiKey;
    std::string model;
    double      threshold = 0.7;   ///< confidence to upgrade MAY -> LLM_SUSPECT
    std::string sliceOutPath = "bof_slices.json";
    std::string verdictPath = "bof_verdicts.json";
    std::string sidecarPath;       ///< python sidecar script; empty => skip LLM
    std::string pythonExe = "python3";

    /// Merge values from a JSON config file (missing keys keep current values).
    /// @return false if the file cannot be read/parsed (caller may then fall
    /// back to environment variables).
    bool loadFromFile(const std::string& path);

    /// Fill any still-empty endpoint fields from BOF_LLM_* environment vars.
    void loadFromEnv();

    /// LLM calls are attempted only when an endpoint and a sidecar are both set.
    /// Otherwise we operate in "API-empty" mode (slices are still exported).
    bool hasApi() const;
};

/// A verdict returned by the sidecar for a single slice id.
struct LLMVerdict {
    std::string id;
    std::string verdict = "UNKNOWN"; ///< OUT_OF_BOUNDS | SAFE | UNKNOWN
    double      confidence = 0.0;
    std::string maxIndexReasoned = "unknown";
    std::string rationale;
};

/// Triage manager: collects slices, always exports them, and optionally drives
/// the sidecar to obtain verdicts.
class LLMTriage {
public:
    void setConfig(const LLMTriageConfig& c) { cfg = c; }
    const LLMTriageConfig& config() const { return cfg; }

    /// Extract a slice for one surviving MAY access (read-only over the IR).
    /// @param base       GEP result / buffer pointer variable.
    /// @param indexVar   the GEP index operand (may be null -> degrade).
    /// @param validRange buffer valid index/byte range (capacity).
    /// @param isHeap     byte-domain (heap) buffer?
    /// @param loc        access ICFG node (for source location).
    /// @param ra         range analysis (reused for affine/token/range queries).
    /// @param out        filled slice on success.
    /// @return false if not enough information to build a meaningful slice.
    bool collectSlice(const SVFVar* base, const SVFVar* indexVar,
                      const Range& validRange, bool isHeap, const ICFGNode* loc,
                      RangeAnalysis& ra, BofSlice& out);

    void addSlice(const BofSlice& s) { slices.push_back(s); }
    bool empty() const { return slices.empty(); }
    size_t size() const { return slices.size(); }
    const std::vector<BofSlice>& getSlices() const { return slices; }

    /// Always write the collected slices to cfg.sliceOutPath (bof-slice/v1).
    /// This is the "API-empty" fallback report and schema self-demo.
    bool writeSlices() const;

    /// If cfg.hasApi(), invoke the sidecar and load back verdicts keyed by id.
    /// Any failure (no api / non-zero exit / missing file) yields an empty map
    /// and is treated as "no verdict" (all accesses stay MAY).
    bool runSidecarAndLoad(std::map<std::string, LLMVerdict>& out) const;

private:
    /// Associate loop guards that constrain @p indexVar by structural-token
    /// matching against every CmpStmt in the IR (best effort; empty on miss).
    void extractGuards(const SVFVar* indexVar, RangeAnalysis& ra,
                       std::vector<GuardInfo>& out) const;

    /// Best-effort induction shape (step magnitude / update operator) by
    /// locating a BinaryOp that feeds the induction slot.
    void extractInduction(const SVFVar* indexVar, RangeAnalysis& ra,
                          InductionInfo& out) const;

    /// Read source lines around (file:line) for the LLM context window.
    std::string readCodeSnippet(const std::string& file, int line) const;

    LLMTriageConfig cfg;
    std::vector<BofSlice> slices;
};

} // namespace SVF

#endif /* LLM_TRIAGE_H_ */

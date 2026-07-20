//===- LLMTriage.cpp -- LLM-assisted MAY-triage overlay for BOF -----------===//
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
 * LLMTriage.cpp
 *
 *  Created on: JUN 12, 2026
 *      Author: Yaokun Yang
 */

#include "BOF/LLMTriage.h"
#include "BOF/BofAlertReporter.h"
#include "SABER/SaberSemanticRules.h"
#include "BOF/RangeAnalysis.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/SVFVariables.h"
#include "Graphs/ICFGNode.h"
#include "Util/SourceEvidence.h"
#include "Util/cJSON.h"
#include "Util/SVFUtil.h"
#include "Util/UnifiedAlertWriter.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace SVF;

namespace {

/// Render an interval bound, mapping the sentinels to readable strings so the
/// JSON never contains a non-finite numeric literal. Uses a tolerant check
/// because arithmetic on the saturated sentinels (negate/sext) can yield
/// NINF+1 / INF-1 rather than the exact constants (mirrors Range::toString).
std::string boundStr(Range::BoundType v)
{
    if (v >= Range::INF - 1024)
        return "INF";
    if (v <= Range::NINF + 1024)
        return "-INF";
    return std::to_string(v);
}

/// Minimal JSON string escaping for hand-written serialisation.
std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c & 0xff);
                out += buf;
            }
            else
                out += c;
        }
    }
    return out;
}

/// Trim the SVF value-name annotation suffix (e.g. " (argument valvar)").
std::string friendlyName(const SVFVar* var)
{
    if (!var)
        return "unknown";
    std::string n = var->getValueName();
    const size_t p = n.find(" (");
    if (p != std::string::npos)
        n = n.substr(0, p);
    if (n.empty())
        n = "%" + std::to_string(var->getId());
    return n;
}

/// Map an ICmp predicate code to its source-level operator.
std::string predicateStr(u32_t pred)
{
    switch (pred)
    {
    case CmpStmt::ICMP_EQ:  return "==";
    case CmpStmt::ICMP_NE:  return "!=";
    case CmpStmt::ICMP_UGT:
    case CmpStmt::ICMP_SGT: return ">";
    case CmpStmt::ICMP_UGE:
    case CmpStmt::ICMP_SGE: return ">=";
    case CmpStmt::ICMP_ULT:
    case CmpStmt::ICMP_SLT: return "<";
    case CmpStmt::ICMP_ULE:
    case CmpStmt::ICMP_SLE: return "<=";
    default:                return "?";
    }
}

/// Format a symbolic affine form vector as a human/LLM-readable expression.
std::string affineStr(const std::vector<AffineTerm>& terms)
{
    if (terms.empty())
        return "unknown";
    std::ostringstream os;
    bool first = true;
    for (const AffineTerm& t : terms)
    {
        if (!first)
            os << " | ";
        first = false;
        if (t.base.empty())
            os << t.offset;
        else if (t.offset == 0)
            os << t.base;
        else
            os << t.base << (t.offset > 0 ? "+" : "") << t.offset;
    }
    return os.str();
}

/// Read a whole text file into a string. Returns false if unreadable.
bool slurp(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

// ===========================================================================
// SliceRange
// ===========================================================================
SliceRange SliceRange::from(const Range& r)
{
    SliceRange sr;
    sr.isTop = r.isTop();
    sr.isBottom = r.isBottom();
    sr.lower = boundStr(r.getLower());
    sr.upper = boundStr(r.getUpper());
    return sr;
}

static std::string sliceRangeJson(const SliceRange& r)
{
    std::ostringstream os;
    os << "{ \"lower\": \"" << jsonEscape(r.lower) << "\", \"upper\": \""
       << jsonEscape(r.upper) << "\", \"is_top\": " << (r.isTop ? "true" : "false")
       << ", \"is_bottom\": " << (r.isBottom ? "true" : "false") << " }";
    return os.str();
}

// ===========================================================================
// BofSlice serialisation
// ===========================================================================
std::string BofSlice::toJson(const std::string& ind) const
{
    const std::string i2 = ind + "  ";
    const std::string i3 = i2 + "  ";
    std::ostringstream os;
    os << ind << "{\n";
    os << i2 << "\"id\": \"" << jsonEscape(id) << "\",\n";
    os << i2 << "\"kind\": \"" << jsonEscape(kind) << "\",\n";
    os << i2 << "\"static_verdict\": \"" << jsonEscape(staticVerdict) << "\",\n";

    os << i2 << "\"access\": {\n";
    os << i3 << "\"file\": \"" << jsonEscape(file) << "\",\n";
    os << i3 << "\"line\": " << line << ",\n";
    os << i3 << "\"col\": " << col << ",\n";
    os << i3 << "\"function\": \"" << jsonEscape(function) << "\",\n";
    os << i3 << "\"base\": \"" << jsonEscape(base) << "\",\n";
    os << i3 << "\"ir\": \"" << jsonEscape(ir) << "\",\n";
    os << i3 << "\"index_expr\": \"" << jsonEscape(indexExpr) << "\",\n";
    os << i3 << "\"index_range_static\": " << sliceRangeJson(indexRange) << ",\n";
    os << i3 << "\"access_range\": " << sliceRangeJson(accessRange) << "\n";
    os << i2 << "},\n";

    os << i2 << "\"buffer\": {\n";
    os << i3 << "\"capacity\": " << sliceRangeJson(capacity) << ",\n";
    os << i3 << "\"is_heap\": " << (isHeap ? "true" : "false") << ",\n";
    os << i3 << "\"domain\": \"" << jsonEscape(domain) << "\",\n";
    os << i3 << "\"access_kind\": \"" << jsonEscape(reportKind) << "\"\n";
    os << i2 << "},\n";

    os << i2 << "\"induction\": {\n";
    os << i3 << "\"var\": \"" << jsonEscape(induction.var) << "\",\n";
    os << i3 << "\"init\": \"" << jsonEscape(induction.init) << "\",\n";
    os << i3 << "\"step\": \"" << jsonEscape(induction.step) << "\",\n";
    os << i3 << "\"update_op\": \"" << jsonEscape(induction.updateOp) << "\"\n";
    os << i2 << "},\n";

    os << i2 << "\"guards\": [";
    for (size_t k = 0; k < guards.size(); ++k)
    {
        const GuardInfo& g = guards[k];
        os << (k ? "," : "") << "\n" << i3 << "{ \"predicate\": \""
           << jsonEscape(g.predicate) << "\", \"lhs\": \"" << jsonEscape(g.lhs)
           << "\", \"rhs\": \"" << jsonEscape(g.rhs) << "\", \"rhs_range\": "
           << sliceRangeJson(g.rhsRange) << ", \"enter_loop_when\": \""
           << jsonEscape(g.enterLoopWhen) << "\", \"valid\": "
           << (g.valid ? "true" : "false") << " }";
    }
    os << (guards.empty() ? "" : ("\n" + i2)) << "],\n";

    os << i2 << "\"code_snippet\": \"" << jsonEscape(codeSnippet) << "\"\n";
    os << ind << "}";
    return os.str();
}

// ===========================================================================
// LLMTriageConfig
// ===========================================================================
bool LLMTriageConfig::loadFromFile(const std::string& path)
{
    std::string text;
    if (!slurp(path, text))
        return false;
    cJSON* root = cJSON_Parse(text.c_str());
    if (!root)
        return false;

    auto getStr = [&](const char* key, std::string& dst) {
        cJSON* it = cJSON_GetObjectItem(root, key);
        if (it && cJSON_IsString(it) && it->valuestring)
            dst = it->valuestring;
    };
    getStr("api_url", apiUrl);
    getStr("api_key", apiKey);
    getStr("model", model);
    getStr("sidecar", sidecarPath);
    getStr("slice_out", sliceOutPath);
    getStr("verdict_out", verdictPath);
    getStr("python", pythonExe);

    cJSON* th = cJSON_GetObjectItem(root, "threshold");
    if (th && cJSON_IsNumber(th))
        threshold = th->valuedouble;

    cJSON_Delete(root);
    return true;
}

void LLMTriageConfig::loadFromEnv()
{
    if (apiUrl.empty())
        if (const char* v = std::getenv("BOF_LLM_API_URL"))
            apiUrl = v;
    if (apiKey.empty())
        if (const char* v = std::getenv("BOF_LLM_API_KEY"))
            apiKey = v;
    if (model.empty())
        if (const char* v = std::getenv("BOF_LLM_MODEL"))
            model = v;
}

bool LLMTriageConfig::hasApi() const
{
    return !apiUrl.empty() && !sidecarPath.empty();
}

// ===========================================================================
// LLMTriage: slice extraction
// ===========================================================================
bool LLMTriage::collectSlice(const SVFVar* base, const SVFVar* indexVar,
                             const Range& validRange, bool isHeap,
                             const ICFGNode* loc, RangeAnalysis& ra,
                             BofSlice& out)
{
    if (!loc)
        return false;

    // ---- access point (parse SVF's getSourceLoc JSON: {"ln","cl","fl"}) ----
    const SourceLocation location = parseSourceLocation(loc->getSourceLoc());
    out.file = location.file;
    out.line = location.line;
    out.col = location.column;
    if (loc->getFun())
        out.function = loc->getFun()->getName();

    out.kind = "GEP_OOB";
    out.staticVerdict = "MAY";
    out.id = out.kind + "@" + out.file + ":" + std::to_string(out.line) + ":" +
             std::to_string(out.col);
    out.base = friendlyName(base);
    out.ir = base ? base->toString() : "";

    // ---- index symbolic form + static range ----
    if (indexVar)
    {
        out.indexExpr = affineStr(ra.analyzeAffine(indexVar));
        out.semanticKey = SaberSemanticRules::valueKey(indexVar);
        const SourceLocation semanticLocation = parseSourceLocation(indexVar->getSourceLoc());
        out.semanticFile = semanticLocation.file;
        out.semanticLine = semanticLocation.line;
        out.indexRange = SliceRange::from(ra.analyzeVarRange(indexVar));
    }
    else
    {
        out.indexExpr = "unknown";
        out.indexRange = SliceRange::from(Range::TOP);
    }

    // ---- buffer capacity ----
    out.capacity = SliceRange::from(validRange);
    out.isHeap = isHeap;
    out.domain = isHeap ? "bytes" : "elements";

    // ---- loop structure (best effort) ----
    out.induction = InductionInfo();
    out.induction.var = friendlyName(indexVar);
    out.guards.clear();
    if (indexVar)
    {
        extractInduction(indexVar, ra, out.induction);
        extractGuards(indexVar, ra, out.guards);
    }

    // ---- raw source context ----
    out.codeSnippet = readCodeSnippet(out.file, out.line);

    return true;
}

void LLMTriage::extractGuards(const SVFVar* indexVar, RangeAnalysis& ra,
                              std::vector<GuardInfo>& out) const
{
    const std::string idxTok = ra.locationToken(indexVar);
    if (idxTok.empty())
        return;

    SVFIR* pag = SVFIR::getPAG();
    std::set<std::string> seen;
    for (SVFStmt* stmt : pag->getSVFStmtSet(SVFStmt::Cmp))
    {
        const CmpStmt* cmp = SVFUtil::dyn_cast<CmpStmt>(stmt);
        if (!cmp || cmp->getOpVarNum() < 2)
            continue;

        const SVFVar* op0 = cmp->getOpVar(0);
        const SVFVar* op1 = cmp->getOpVar(1);
        const std::string t0 = ra.locationToken(op0);
        const std::string t1 = ra.locationToken(op1);

        const bool m0 = (!t0.empty() && t0 == idxTok);
        const bool m1 = (!t1.empty() && t1 == idxTok);
        if (!m0 && !m1)
            continue;

        // Orient so that lhs is the index side, rhs is the bound side.
        const SVFVar* idxSide = m0 ? op0 : op1;
        const SVFVar* bndSide = m0 ? op1 : op0;

        GuardInfo g;
        g.predicate = predicateStr(cmp->getPredicate());
        // If the index is on the rhs (e.g. `10 >= i`), the printed predicate is
        // w.r.t. op0; we keep it raw but record the index expression on lhs for
        // the LLM, which also sees the code snippet for disambiguation.
        g.lhs = friendlyName(idxSide);
        g.rhs = affineStr(ra.analyzeAffine(bndSide));
        if (g.rhs == "unknown")
            g.rhs = friendlyName(bndSide);
        g.rhsRange = SliceRange::from(ra.analyzeVarRange(bndSide));
        g.enterLoopWhen = "unknown";
        g.valid = true;

        const std::string key = g.predicate + "#" + g.lhs + "#" + g.rhs;
        if (seen.insert(key).second)
            out.push_back(g);
    }
}

void LLMTriage::extractInduction(const SVFVar* indexVar, RangeAnalysis& ra,
                                 InductionInfo& out) const
{
    const std::string idxTok = ra.locationToken(indexVar);
    if (idxTok.empty())
        return;

    SVFIR* pag = SVFIR::getPAG();
    for (SVFStmt* stmt : pag->getSVFStmtSet(SVFStmt::BinaryOp))
    {
        const BinaryOPStmt* bin = SVFUtil::dyn_cast<BinaryOPStmt>(stmt);
        if (!bin || bin->getOpVarNum() < 2)
            continue;
        const u32_t oc = bin->getOpcode();
        const bool isAdd = (oc == BinaryOPStmt::Add);
        const bool isSub = (oc == BinaryOPStmt::Sub);
        if (!isAdd && !isSub)
            continue;

        const SVFVar* a = bin->getOpVar(0);
        const SVFVar* b = bin->getOpVar(1);
        const std::string ta = ra.locationToken(a);
        const std::string tb = ra.locationToken(b);
        const bool ma = (!ta.empty() && ta == idxTok);
        const bool mb = (!tb.empty() && tb == idxTok);
        if (!ma && !mb)
            continue;

        const SVFVar* other = ma ? b : a;
        const Range r = ra.analyzeVarRange(other);
        if (r.isConstant())
        {
            out.step = std::to_string(r.getLower());
            out.updateOp = isAdd ? "+" : "-";
            return; // first confident match wins
        }
    }
}

std::string LLMTriage::readCodeSnippet(const std::string& file, int line) const
{
    return readSourceSnippet(file, line, 8, 2);
}

// ===========================================================================
// LLMTriage: export + sidecar
// ===========================================================================
bool LLMTriage::writeSlices() const
{
    std::ofstream os(cfg.sliceOutPath, std::ios::trunc);
    if (!os)
    {
        SVFUtil::errs() << "[LLMTriage] cannot open slice output: "
                        << cfg.sliceOutPath << "\n";
        return false;
    }

    os << "{\n";
    os << "  \"schema\": \"bof-slice/v1\",\n";
    os << "  \"generated_by\": \"SVFmemplus-BOF\",\n";
    os << "  \"slice_count\": " << slices.size() << ",\n";
    os << "  \"slices\": [";
    for (size_t k = 0; k < slices.size(); ++k)
    {
        os << (k ? "," : "") << "\n" << slices[k].toJson("    ");
    }
    os << (slices.empty() ? "" : "\n  ") << "]\n";
    os << "}\n";
    return true;
}

bool BofAlertReporter::write(const LLMTriageConfig& cfg,
                             const std::vector<BofSlice>& slices)
{
    if (cfg.alertOutDir.empty())
        return false;

    UnifiedAlertWriter writer(
        (std::filesystem::path(cfg.alertOutDir) / "alerts").string(),
        "bof");
    bool ok = true;
    for (const BofSlice& s : slices)
    {
        std::ostringstream os;
        os << "{\n"
           << "  \"access\": {\n";
        if (!s.file.empty() && s.line > 0)
            os << "    \"location\": { \"file\": \"" << jsonEscape(s.file)
               << "\", \"line\": " << s.line << ", \"column\": " << s.col
               << " },\n";
        if (!s.function.empty())
            os << "    \"function\": \"" << jsonEscape(s.function) << "\",\n";
        os
           << "    \"kind\": \"" << jsonEscape(s.kind) << "\",\n"
           << "    \"base\": \"" << jsonEscape(s.base) << "\",\n"
           << "    \"ir\": \"" << jsonEscape(s.ir) << "\",\n"
           << "    \"index_or_length_expression\": \"" << jsonEscape(s.indexExpr) << "\",\n"
           << "    \"range\": " << sliceRangeJson(s.accessRange);
        if (!s.codeSnippet.empty())
            os << ",\n    \"source_context\": \"" << jsonEscape(s.codeSnippet) << "\"";
        os << "\n"
           << "  },\n"
           << "  \"buffer\": {\n"
           << "    \"capacity\": " << sliceRangeJson(s.capacity) << ",\n"
           << "    \"is_heap\": " << (s.isHeap ? "true" : "false") << ",\n"
           << "    \"domain\": \"" << jsonEscape(s.domain) << "\"\n"
           << "  },\n"
           << "  \"variables\": [\n"
           << "    { \"expression\": \"" << jsonEscape(s.indexExpr)
           << "\", \"initial_range\": " << sliceRangeJson(s.indexRange)
           << ", \"final_range\": " << sliceRangeJson(s.accessRange);
        if (!s.induction.var.empty() && s.induction.var != "unknown")
            os << ", \"name\": \"" << jsonEscape(s.induction.var) << "\"";
        if (!s.semanticKey.empty())
            os << ", \"semantic_key\": \"" << jsonEscape(s.semanticKey) << "\"";
        if (!s.semanticFile.empty() && s.semanticLine > 0)
            os << ", \"semantic_location\": { \"file\": \""
               << jsonEscape(s.semanticFile) << "\", \"line\": "
               << s.semanticLine << " }";
        os << " }\n"
           << "  ],\n"
           << "  \"range_analysis\": [\n"
           << "    { \"operation\": \"range_seed\", \"inputs\": ["
           << "{ \"variable\": \"" << jsonEscape(s.indexExpr)
           << "\", \"range\": " << sliceRangeJson(s.indexRange)
           << " }], \"constraint\": \"none\", \"output_range\": "
           << sliceRangeJson(s.indexRange)
           << ", \"explanation\": \"Seed the index or transfer-length range.\" }";

        if (s.induction.var != "unknown")
            os << ",\n    { \"operation\": \"induction_propagation\", \"inputs\": ["
               << "{ \"variable\": \"" << jsonEscape(s.induction.var)
               << "\", \"init\": \"" << jsonEscape(s.induction.init)
               << "\", \"step\": \"" << jsonEscape(s.induction.step)
               << "\" }], \"constraint\": \"" << jsonEscape(s.induction.updateOp)
               << jsonEscape(s.induction.step) << "\", \"output_range\": "
               << sliceRangeJson(s.indexRange)
               << ", \"explanation\": \"Propagate the induction recurrence.\" }";
        for (const GuardInfo& g : s.guards)
            os << ",\n    { \"operation\": \"guard_narrowing\", \"inputs\": ["
               << "{ \"variable\": \"" << jsonEscape(g.lhs) << "\", \"range\": "
               << sliceRangeJson(s.indexRange) << " }, { \"variable\": \""
               << jsonEscape(g.rhs) << "\", \"range\": "
               << sliceRangeJson(g.rhsRange) << " }], \"constraint\": \""
               << jsonEscape(g.lhs + " " + g.predicate + " " + g.rhs)
               << "\", \"output_range\": " << sliceRangeJson(s.indexRange)
               << ", \"explanation\": \"Apply a controlling comparison.\" }";
        os << ",\n    { \"operation\": \"access_range\", \"inputs\": ["
           << "{ \"base\": \"" << jsonEscape(s.base) << "\" }, "
           << "{ \"index_or_length\": \"" << jsonEscape(s.indexExpr)
           << "\" }], \"constraint\": \"domain=" << jsonEscape(s.domain)
           << "\", \"output_range\": " << sliceRangeJson(s.accessRange)
           << ", \"explanation\": \"Compute the final byte or element range touched.\" },\n"
           << "    { \"operation\": \"bounds_comparison\", \"inputs\": ["
           << "{ \"access_range\": " << sliceRangeJson(s.accessRange) << " }, "
           << "{ \"valid_range\": " << sliceRangeJson(s.capacity)
           << " }], \"constraint\": \"access_range must be contained in valid_range\", "
           << "\"output_range\": " << sliceRangeJson(s.accessRange)
           << ", \"explanation\": \"Static checker verdict: "
           << jsonEscape(s.staticVerdict) << ".\" }\n"
           << "  ],\n"
           << "  \"evidence\": { \"checker\": { \"verdict\": \""
           << jsonEscape(s.staticVerdict)
           << "\", \"analysis_degraded\": " << (s.indexRange.isTop ? "true" : "false")
           << " } }\n"
           << "}\n";

        ok = writer.write("svfmemplus", os.str()) && ok;
    }

    return writer.removeStale() && ok;
}

bool LLMTriage::runSidecarAndLoad(std::map<std::string, LLMVerdict>& out) const
{
    out.clear();
    if (!cfg.hasApi())
        return false;

    // Pass the endpoint via environment so the API key never appears on the
    // command line; the sidecar inherits it through std::system.
    if (!cfg.apiUrl.empty())
        ::setenv("BOF_LLM_API_URL", cfg.apiUrl.c_str(), 1);
    if (!cfg.apiKey.empty())
        ::setenv("BOF_LLM_API_KEY", cfg.apiKey.c_str(), 1);
    if (!cfg.model.empty())
        ::setenv("BOF_LLM_MODEL", cfg.model.c_str(), 1);

    std::ostringstream cmd;
    cmd << cfg.pythonExe << " \"" << cfg.sidecarPath << "\""
        << " --slices \"" << cfg.sliceOutPath << "\""
        << " --out \"" << cfg.verdictPath << "\""
        << " --threshold " << cfg.threshold;

    SVFUtil::outs() << "[LLMTriage] invoking sidecar: " << cmd.str() << "\n";
    const int rc = std::system(cmd.str().c_str());
    if (rc != 0)
    {
        SVFUtil::errs() << "[LLMTriage] sidecar exited with code " << rc
                        << "; keeping all reports as MAY.\n";
        return false;
    }

    std::string text;
    if (!slurp(cfg.verdictPath, text))
    {
        SVFUtil::errs() << "[LLMTriage] verdict file missing: " << cfg.verdictPath
                        << "\n";
        return false;
    }
    cJSON* root = cJSON_Parse(text.c_str());
    if (!root)
        return false;

    cJSON* arr = cJSON_GetObjectItem(root, "verdicts");
    if (arr && cJSON_IsArray(arr))
    {
        cJSON* el = nullptr;
        cJSON_ArrayForEach(el, arr)
        {
            if (!cJSON_IsObject(el))
                continue;
            LLMVerdict v;
            if (cJSON* id = cJSON_GetObjectItem(el, "id"))
                if (cJSON_IsString(id) && id->valuestring) v.id = id->valuestring;
            if (cJSON* vd = cJSON_GetObjectItem(el, "verdict"))
                if (cJSON_IsString(vd) && vd->valuestring) v.verdict = vd->valuestring;
            if (cJSON* cf = cJSON_GetObjectItem(el, "confidence"))
                if (cJSON_IsNumber(cf)) v.confidence = cf->valuedouble;
            if (cJSON* mi = cJSON_GetObjectItem(el, "max_index_reasoned"))
                if (cJSON_IsString(mi) && mi->valuestring) v.maxIndexReasoned = mi->valuestring;
            if (cJSON* ra = cJSON_GetObjectItem(el, "rationale"))
                if (cJSON_IsString(ra) && ra->valuestring) v.rationale = ra->valuestring;
            if (!v.id.empty())
                out[v.id] = v;
        }
    }
    cJSON_Delete(root);
    return !out.empty();
}

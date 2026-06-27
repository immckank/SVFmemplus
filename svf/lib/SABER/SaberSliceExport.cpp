//===- SaberSliceExport.cpp -- SABER alert context export for downstream LLM -===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

#include "SABER/SaberSliceExport.h"
#include "Graphs/ICFGNode.h"
#include "Util/cJSON.h"
#include "Util/SVFUtil.h"

#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>

using namespace SVF;

namespace {

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

// Reduce an SVF type repr to a stable, human-readable key for grouping, e.g.
// "%class.FalconLog" / "class.FalconLog = { ... }" -> "FalconLog",
// "%\"class.std::__cxx11::basic_string\"" -> "std::__cxx11::basic_string".
std::string cleanTypeName(std::string s)
{
    // Drop anything after the first '=' or '{' (field expansions) and quotes.
    for (char delim : {'=', '{'})
    {
        size_t p = s.find(delim);
        if (p != std::string::npos)
            s = s.substr(0, p);
    }
    std::string out;
    for (char c : s)
        if (c != '%' && c != '"' && c != '\'')
            out += c;
    // Trim whitespace.
    size_t a = out.find_first_not_of(" \t\n");
    size_t b = out.find_last_not_of(" \t\n");
    if (a == std::string::npos)
        return "";
    out = out.substr(a, b - a + 1);
    // Strip SVF kind tag ("S." for struct/aggregate reprs) then the C++ tag.
    if (out.compare(0, 2, "S.") == 0)
        out = out.substr(2);
    for (const char* pfx : {"class.", "struct.", "union."})
    {
        size_t L = std::string(pfx).size();
        if (out.compare(0, L, pfx) == 0)
        {
            out = out.substr(L);
            break;
        }
    }
    return out;
}

} // namespace

namespace SVF {

const char* saberSliceKindStr(SaberSliceKind kind)
{
    switch (kind)
    {
    case SaberSliceKind::USE_AFTER_FREE: return "USE_AFTER_FREE";
    case SaberSliceKind::UNINIT_USE:     return "UNINIT_USE";
    case SaberSliceKind::MEMORY_LEAK:    return "MEMORY_LEAK";
    case SaberSliceKind::DOUBLE_FREE:    return "DOUBLE_FREE";
    }
    return "UNKNOWN";
}

std::string SaberSlice::toJson(const std::string& ind) const
{
    const std::string i2 = ind + "  ";
    const std::string i3 = i2 + "  ";
    std::ostringstream os;
    os << ind << "{\n";
    os << i2 << "\"id\": \"" << jsonEscape(id) << "\",\n";
    os << i2 << "\"kind\": \"" << saberSliceKindStr(kind) << "\",\n";
    os << i2 << "\"static_verdict\": \"" << jsonEscape(staticVerdict) << "\",\n";
    os << i2 << "\"source\": { \"file\": \"" << jsonEscape(sourceFile) << "\", \"line\": "
       << sourceLine << ", \"col\": " << sourceCol << " },\n";

    if (kind == SaberSliceKind::USE_AFTER_FREE)
    {
        os << i2 << "\"report_kind\": \"" << jsonEscape(reportKind) << "\",\n";
        os << i2 << "\"free\": { \"file\": \"" << jsonEscape(freeFile) << "\", \"line\": "
           << freeLine << " },\n";
        os << i2 << "\"use\": { \"file\": \"" << jsonEscape(useFile) << "\", \"line\": "
           << useLine << " },\n";
    }
    else if (kind == SaberSliceKind::UNINIT_USE)
    {
        os << i2 << "\"source_kind\": \"" << jsonEscape(sourceKind) << "\",\n";
        os << i2 << "\"object_type\": \"" << jsonEscape(objectType) << "\",\n";
        os << i2 << "\"variable\": \"" << jsonEscape(variable) << "\",\n";
        os << i2 << "\"source_func\": \"" << jsonEscape(sourceFunc) << "\",\n";
        os << i2 << "\"caller\": { \"file\": \"" << jsonEscape(callerFile) << "\", \"line\": "
           << callerLine << " },\n";
        os << i2 << "\"use\": { \"file\": \"" << jsonEscape(useFile) << "\", \"line\": "
           << useLine << ", \"col\": " << useCol << " },\n";
        os << i2 << "\"allocator\": \"" << jsonEscape(allocator) << "\",\n";
        os << i2 << "\"zeroing\": " << (zeroing ? "true" : "false") << ",\n";
    }
    else if (kind == SaberSliceKind::MEMORY_LEAK)
    {
        os << i2 << "\"leak_kind\": \"" << jsonEscape(leakKind) << "\",\n";
    }
    else if (kind == SaberSliceKind::DOUBLE_FREE)
    {
        os << i2 << "\"free\": { \"file\": \"" << jsonEscape(freeFile) << "\", \"line\": "
           << freeLine << " },\n";
        os << i2 << "\"free2\": { \"file\": \"" << jsonEscape(free2File) << "\", \"line\": "
           << free2Line << " },\n";
    }

    os << i2 << "\"path_conditions\": [";
    for (size_t k = 0; k < pathConditions.size(); ++k)
    {
        const SaberPathEdge& e = pathConditions[k];
        os << (k ? "," : "") << "\n" << i3 << "{ \"location\": \"" << jsonEscape(e.location)
           << "\", \"condition\": \"" << jsonEscape(e.condition) << "\" }";
    }
    os << (pathConditions.empty() ? "" : ("\n" + i2)) << "],\n";
    os << i2 << "\"code_snippet\": \"" << jsonEscape(codeSnippet) << "\"\n";
    os << ind << "}";
    return os.str();
}

bool SaberSliceExportUtil::parseLocField(const std::string& locJson, const char* key,
                                         std::string& outStr, int& outInt)
{
    cJSON* lj = cJSON_Parse(locJson.c_str());
    if (!lj)
        return false;
    bool ok = false;
    if (cJSON* fl = cJSON_GetObjectItem(lj, key))
    {
        if (cJSON_IsString(fl) && fl->valuestring)
        {
            outStr = fl->valuestring;
            ok = true;
        }
        else if (cJSON_IsNumber(fl))
        {
            outInt = static_cast<int>(fl->valuedouble);
            ok = true;
        }
    }
    if (!ok && std::string(key) == "ln")
    {
        if (cJSON* fl = cJSON_GetObjectItem(lj, "line"))
        {
            if (cJSON_IsNumber(fl))
            {
                outInt = static_cast<int>(fl->valuedouble);
                ok = true;
            }
        }
    }
    cJSON_Delete(lj);
    return ok;
}

bool SaberSliceExportUtil::parseLocFileLine(const std::string& locJson, std::string& file,
                                           u32_t& line)
{
    int ln = 0;
    const bool ok = parseLocFileLine(locJson, file, ln);
    line = static_cast<u32_t>(ln);
    return ok;
}

bool SaberSliceExportUtil::parseLocFileLine(const std::string& locJson, std::string& file,
                                            int& line)
{
    file.clear();
    line = 0;
    if (!parseLocField(locJson, "fl", file, line))
        parseLocField(locJson, "file", file, line);
    parseLocField(locJson, "ln", file, line);
    return !file.empty() || line != 0;
}

SaberSourceLoc SaberSliceExportUtil::locFromICFG(const ICFGNode* icfg)
{
    SaberSourceLoc loc;
    fillLocFromICFG(icfg, loc.file, loc.line, loc.col);
    return loc;
}

void SaberSliceExportUtil::fillLocFromICFG(const ICFGNode* icfg, std::string& file, int& line,
                                           int& col)
{
    if (icfg == nullptr)
        return;
    const std::string& sloc = icfg->getSourceLoc();
    parseLocField(sloc, "fl", file, line);
    if (file.empty())
        parseLocField(sloc, "file", file, line);
    int dummy = 0;
    parseLocField(sloc, "ln", file, line);
    parseLocField(sloc, "cl", file, col);
    (void)dummy;
}

std::string SaberSliceExportUtil::makeICFGPairLocKey(const ICFGNode* lhs, const ICFGNode* rhs)
{
    std::string key;
    if (lhs)
        key += lhs->getSourceLoc();
    key += "#";
    if (rhs)
        key += rhs->getSourceLoc();
    return key;
}

void SaberSliceExportUtil::collectPathConditions(const GenericBug::EventStack& eventStack,
                                                 std::vector<SaberPathEdge>& out)
{
    out.clear();
    auto lastIt = eventStack.end();
    if (eventStack.size() > 1)
        --lastIt;
    for (auto it = eventStack.begin(); it != lastIt; ++it)
    {
        SaberPathEdge edge;
        edge.location = it->getEventLoc();
        edge.condition = it->getEventDescription();
        out.push_back(edge);
    }
}

std::string SaberSliceCollector::readCodeSnippet(const std::string& file, int line) const
{
    if (file.empty() || line <= 0)
        return "";

    std::vector<std::string> candidates;
    candidates.push_back(file);
    const char* root = std::getenv("SABER_SOURCE_ROOT");
    if (root == nullptr || !*root)
        root = std::getenv("UAF_SOURCE_ROOT");
    if (root == nullptr || !*root)
        root = std::getenv("UNINIT_SOURCE_ROOT");
    if (root != nullptr && *root)
    {
        std::string rooted = root;
        if (!rooted.empty() && rooted.back() != '/')
            rooted += '/';
        rooted += file;
        candidates.push_back(rooted);
    }

    for (const std::string& path : candidates)
    {
        std::ifstream in(path);
        if (!in)
            continue;

        const int before = 10;
        const int after = 3;
        const int from = (line - before > 1) ? (line - before) : 1;
        const int to = line + after;

        std::string out;
        std::string cur;
        int n = 0;
        while (std::getline(in, cur))
        {
            ++n;
            if (n < from)
                continue;
            if (n > to)
                break;
            out += cur;
            out += "\n";
        }
        if (!out.empty())
            return out;
    }
    return "";
}

bool SaberSliceCollector::collectUAFSlice(const GenericBug::EventStack& eventStack,
                                          const std::string& reportKind, SaberSlice& out) const
{
    if (eventStack.empty())
        return false;

    out = SaberSlice();
    out.kind = SaberSliceKind::USE_AFTER_FREE;
    out.reportKind = reportKind;

    const ICFGNode* sourceICFG = nullptr;
    const ICFGNode* freeICFG = nullptr;
    const ICFGNode* useICFG = nullptr;

    for (const SVFBugEvent& ev : eventStack)
    {
        if (ev.getEventType() == SVFBugEvent::SourceInst)
            sourceICFG = ev.getEventInst();
        else if (ev.getEventType() == SVFBugEvent::Free)
            freeICFG = ev.getEventInst();
        else if (ev.getEventType() == SVFBugEvent::Use)
            useICFG = ev.getEventInst();
    }

    SaberSliceExportUtil::fillLocFromICFG(sourceICFG, out.sourceFile, out.sourceLine, out.sourceCol);
    SaberSliceExportUtil::fillLocFromICFG(freeICFG, out.freeFile, out.freeLine, out.sourceCol);
    SaberSliceExportUtil::fillLocFromICFG(useICFG, out.useFile, out.useLine, out.useCol);

    SaberSliceExportUtil::collectPathConditions(eventStack, out.pathConditions);

    std::ostringstream id;
    id << out.freeFile << ":" << out.freeLine << "->" << out.useFile << ":" << out.useLine;
    out.id = id.str();

    out.codeSnippet = readCodeSnippet(out.useFile, out.useLine);
    return !out.useFile.empty();
}

bool SaberSliceCollector::collectUninitSlice(const SVFGNode* source,
                                             const GenericBug::EventStack& eventStack,
                                             const std::string& sourceKind,
                                             const std::string& allocator, bool zeroing,
                                             SaberSlice& out) const
{
    if (eventStack.empty())
        return false;

    out = SaberSlice();
    out.kind = SaberSliceKind::UNINIT_USE;
    out.sourceKind = sourceKind;
    out.allocator = allocator;
    out.zeroing = zeroing;

    // The `SourceInst` event carries the best-effort declaration site that
    // reportBug() resolved (e.g. the stack alloca at falcon_meta.cpp:508), which
    // is far more identifiable than the raw AddrSVFGNode ICFG node (typically a
    // location-less function/global anchor). Fall back to the raw source only if
    // no SourceInst event is present.
    const ICFGNode* sourceInstICFG = nullptr;
    for (auto it = eventStack.rbegin(); it != eventStack.rend(); ++it)
    {
        if (it->getEventType() == SVFBugEvent::SourceInst)
        {
            sourceInstICFG = it->getEventInst();
            break;
        }
    }
    // The declaring function is reliably available from the source-creation node
    // (a FunEntryICFGNode for stack objects), even though that node's own location
    // string is the unparseable "function entry: ..." prefix.
    if (sourceInstICFG != nullptr && sourceInstICFG->getFun() != nullptr)
        out.sourceFunc = sourceInstICFG->getFun()->getName();

    // The actual declaration site + variable name live on the source object itself
    // (the alloca's debug info), not on its creation ICFGNode. Recover them from the
    // AddrSVFGNode's PAG object so named locals like "retryBudget" are identifiable.
    if (const AddrSVFGNode* addr = SVFUtil::dyn_cast<AddrSVFGNode>(source))
    {
        if (const SVFVar* objVar = SVFUtil::dyn_cast<SVFVar>(addr->getPAGSrcNode()))
        {
            const std::string& objLoc = objVar->getSourceLoc();
            SaberSliceExportUtil::parseLocFileLine(objLoc, out.sourceFile, out.sourceLine);
            int dummy = 0;
            SaberSliceExportUtil::parseLocField(objLoc, "nm", out.variable, dummy);
            if (const SVFType* ty = objVar->getType())
                out.objectType = cleanTypeName(ty->toString());
        }
    }
    if (std::getenv("SABER_SLICE_DEBUG"))
    {
        SVFUtil::errs() << "[slice-dbg] func=" << out.sourceFunc
                        << " objType=" << out.objectType << "\n";
        if (const AddrSVFGNode* addr = SVFUtil::dyn_cast<AddrSVFGNode>(source))
            SVFUtil::errs() << "   pagSrc=" << addr->getPAGSrcNode()->toString().substr(0, 200)
                            << "\n";
    }
    if (out.sourceFile.empty() && sourceInstICFG != nullptr)
        SaberSliceExportUtil::fillLocFromICFG(sourceInstICFG, out.sourceFile, out.sourceLine,
                                              out.sourceCol);

    const SVFBugEvent* useEvent = nullptr;
    for (auto it = eventStack.rbegin(); it != eventStack.rend(); ++it)
    {
        if (it->getEventType() == SVFBugEvent::Use)
        {
            useEvent = &(*it);
            break;
        }
    }
    if (useEvent == nullptr)
        useEvent = &eventStack.back();

    SaberSliceExportUtil::fillLocFromICFG(useEvent->getEventInst(), out.useFile, out.useLine,
                                         out.useCol);

    if (out.useFile.empty() && out.useLine == 0)
        return false;

    // Best-effort caller anchor: the project-code site in the declaring function
    // where the value enters the (often shared/inlined) sink. Pick the latest path
    // event that lives in the declaring function but is not the declaration itself.
    if (!out.sourceFunc.empty())
    {
        for (auto it = eventStack.rbegin(); it != eventStack.rend(); ++it)
        {
            const ICFGNode* inst = it->getEventInst();
            if (inst == nullptr || inst == sourceInstICFG)
                continue;
            const FunObjVar* fun = inst->getFun();
            if (fun == nullptr || fun->getName() != out.sourceFunc)
                continue;
            std::string f;
            int ln = 0;
            SaberSliceExportUtil::parseLocFileLine(inst->getSourceLoc(), f, ln);
            if (!f.empty() && ln != 0 && !(f == out.sourceFile && ln == out.sourceLine))
            {
                out.callerFile = f;
                out.callerLine = ln;
                break;
            }
        }
    }

    SaberSliceExportUtil::collectPathConditions(eventStack, out.pathConditions);

    // Identify by declaration site + variable so funnel-shared sinks stay distinct.
    if (!out.sourceFile.empty())
        out.id = "UNINIT@" + out.sourceFile + ":" + std::to_string(out.sourceLine) +
                 (out.variable.empty() ? "" : ("#" + out.variable)) +
                 "->" + out.useFile + ":" + std::to_string(out.useLine);
    else
        out.id = "UNINIT@" + out.useFile + ":" + std::to_string(out.useLine) + ":" +
                 std::to_string(out.useCol);
    out.codeSnippet = readCodeSnippet(out.useFile, out.useLine);
    if (out.codeSnippet.empty() && !out.sourceFile.empty())
        out.codeSnippet = readCodeSnippet(out.sourceFile, out.sourceLine);

    return true;
}

bool SaberSliceCollector::collectLeakSlice(GenericBug::BugType bugType,
                                           const GenericBug::EventStack& eventStack,
                                           SaberSlice& out) const
{
    if (eventStack.empty())
        return false;

    out = SaberSlice();
    out.kind = SaberSliceKind::MEMORY_LEAK;
    if (bugType == GenericBug::NEVERFREE)
        out.leakKind = "NEVERFREE";
    else if (bugType == GenericBug::PARTIALLEAK)
        out.leakKind = "PARTIALLEAK";
    else
        out.leakKind = "UNKNOWN";

    for (const SVFBugEvent& ev : eventStack)
    {
        if (ev.getEventType() == SVFBugEvent::SourceInst)
        {
            SaberSliceExportUtil::fillLocFromICFG(ev.getEventInst(), out.sourceFile, out.sourceLine,
                                                  out.sourceCol);
            break;
        }
    }

    SaberSliceExportUtil::collectPathConditions(eventStack, out.pathConditions);

    out.id = "LEAK@" + out.leakKind + "@" + out.sourceFile + ":" +
             std::to_string(out.sourceLine);
    out.codeSnippet = readCodeSnippet(out.sourceFile, out.sourceLine);
    return !out.sourceFile.empty() || out.sourceLine > 0;
}

bool SaberSliceCollector::collectDoubleFreeSlice(const GenericBug::EventStack& eventStack,
                                                 SaberSlice& out) const
{
    if (eventStack.empty())
        return false;

    out = SaberSlice();
    out.kind = SaberSliceKind::DOUBLE_FREE;

    std::vector<const ICFGNode*> freeSites;
    for (const SVFBugEvent& ev : eventStack)
    {
        if (ev.getEventType() == SVFBugEvent::SourceInst)
            SaberSliceExportUtil::fillLocFromICFG(ev.getEventInst(), out.sourceFile, out.sourceLine,
                                                  out.sourceCol);
        else if (ev.getEventType() == SVFBugEvent::Free)
            freeSites.push_back(ev.getEventInst());
    }

    if (freeSites.size() >= 1)
        SaberSliceExportUtil::fillLocFromICFG(freeSites[0], out.freeFile, out.freeLine,
                                              out.sourceCol);
    if (freeSites.size() >= 2)
        SaberSliceExportUtil::fillLocFromICFG(freeSites[1], out.free2File, out.free2Line,
                                              out.sourceCol);

    SaberSliceExportUtil::collectPathConditions(eventStack, out.pathConditions);

    const std::string anchorFile = !out.freeFile.empty() ? out.freeFile : out.sourceFile;
    const int anchorLine = !out.freeFile.empty() ? out.freeLine : out.sourceLine;
    out.id = "DFREE@" + anchorFile + ":" + std::to_string(anchorLine);
    if (!out.free2File.empty())
        out.id += "->" + out.free2File + ":" + std::to_string(out.free2Line);

    out.codeSnippet = readCodeSnippet(anchorFile, anchorLine);
    if (out.codeSnippet.empty() && !out.sourceFile.empty())
        out.codeSnippet = readCodeSnippet(out.sourceFile, out.sourceLine);

    return !anchorFile.empty() || anchorLine > 0;
}

bool SaberSliceCollector::collectSliceForPending(const SaberPendingReport& pending,
                                                 SaberSlice& out) const
{
    switch (pending.bugType)
    {
    case GenericBug::USEAFTERFREE:
        return collectUAFSlice(pending.eventStack, pending.uafReportKind, out);
    case GenericBug::UNINIT:
        return collectUninitSlice(pending.uninitSource, pending.eventStack,
                                  pending.uninitSourceKind, pending.uninitAllocator,
                                  pending.uninitZeroing, out);
    case GenericBug::DOUBLEFREE:
        return collectDoubleFreeSlice(pending.eventStack, out);
    case GenericBug::NEVERFREE:
    case GenericBug::PARTIALLEAK:
        return collectLeakSlice(pending.bugType, pending.eventStack, out);
    default:
        return false;
    }
}

bool SaberSliceCollector::writeSlices(const char* generatedBy) const
{
    std::ofstream os(sliceOutPath, std::ios::trunc);
    if (!os)
    {
        SVFUtil::errs() << "[SaberSliceExport] cannot open slice output: " << sliceOutPath << "\n";
        return false;
    }

    os << "{\n";
    os << "  \"schema\": \"saber-slice/v1\",\n";
    os << "  \"generated_by\": \"" << jsonEscape(generatedBy ? generatedBy : "SVFmemplus-SABER")
       << "\",\n";
    os << "  \"slice_count\": " << slices.size() << ",\n";

    // Function-level grouping so the offline LLM triage can classify all suspect
    // objects of one function in a single call instead of one call per report.
    // This is the dominant lever for the logger-funnel uninit reports, which share
    // a sink (logging.cpp) but spread over ~one source function each; no report is
    // dropped — every slice index remains a member of exactly one group.
    // Strategy 2: key by the object TYPE (the constructor / creation-site concept),
    // which collapses the many-creations -> one-sink logger funnel that per-function
    // grouping cannot. Each group lists its members with their distinct caller
    // contexts (the LOG call sites) so no report is lost and density is high.
    std::vector<std::string> groupOrder;
    std::map<std::string, std::vector<size_t>> groups;
    for (size_t k = 0; k < slices.size(); ++k)
    {
        std::string key = slices[k].objectType;
        if (key.empty())
            key = slices[k].sourceFunc;
        if (key.empty())
            key = slices[k].id; // singleton group when nothing better is known
        auto it = groups.find(key);
        if (it == groups.end())
        {
            groups.emplace(key, std::vector<size_t>{k});
            groupOrder.push_back(key);
        }
        else
            it->second.push_back(k);
    }
    os << "  \"group_count\": " << groupOrder.size() << ",\n";
    os << "  \"groups\": [";
    for (size_t g = 0; g < groupOrder.size(); ++g)
    {
        const std::vector<size_t>& members = groups[groupOrder[g]];
        // Distinct caller contexts (file:line) within this object-type group.
        std::vector<std::string> ctxs;
        for (size_t idx : members)
        {
            if (slices[idx].callerLine == 0 && slices[idx].callerFile.empty())
                continue;
            std::string c = slices[idx].callerFile + ":" + std::to_string(slices[idx].callerLine);
            if (std::find(ctxs.begin(), ctxs.end(), c) == ctxs.end())
                ctxs.push_back(c);
        }
        os << (g ? "," : "") << "\n    { \"object_type\": \""
           << jsonEscape(groupOrder[g]) << "\", \"member_count\": " << members.size()
           << ", \"caller_contexts\": [";
        for (size_t c = 0; c < ctxs.size(); ++c)
            os << (c ? ", " : "") << "\"" << jsonEscape(ctxs[c]) << "\"";
        os << "], \"members\": [";
        for (size_t m = 0; m < members.size(); ++m)
            os << (m ? ", " : "") << members[m];
        os << "] }";
    }
    os << (groupOrder.empty() ? "" : "\n  ") << "],\n";

    os << "  \"slices\": [";
    for (size_t k = 0; k < slices.size(); ++k)
        os << (k ? "," : "") << "\n" << slices[k].toJson("    ");
    os << (slices.empty() ? "" : "\n  ") << "]\n";
    os << "}\n";
    return true;
}

} // namespace SVF

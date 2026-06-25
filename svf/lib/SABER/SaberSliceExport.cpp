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

    if (source)
        SaberSliceExportUtil::fillLocFromICFG(source->getICFGNode(), out.sourceFile,
                                              out.sourceLine, out.sourceCol);

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

    SaberSliceExportUtil::collectPathConditions(eventStack, out.pathConditions);

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
    os << "  \"slices\": [";
    for (size_t k = 0; k < slices.size(); ++k)
        os << (k ? "," : "") << "\n" << slices[k].toJson("    ");
    os << (slices.empty() ? "" : "\n  ") << "]\n";
    os << "}\n";
    return true;
}

} // namespace SVF

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
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
#include <regex>
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

std::string canonicalIR(std::string text)
{
    text = std::regex_replace(text, std::regex("(ICFGNode|SVFGNode|Var)[0-9]+"), "$1");
    text = std::regex_replace(text, std::regex("%[0-9]+"), "%tmp");
    text = std::regex_replace(text, std::regex("!dbg ![0-9]+"), "!dbg");
    text = std::regex_replace(text, std::regex("ValVar ID: [0-9]+"), "ValVar");
    text = std::regex_replace(text, std::regex("[ \\t]+"), " ");
    return text;
}

void collectTrace(const GenericBug::EventStack& events, std::vector<SaberTraceNode>& out)
{
    out.clear();
    for (const SVFBugEvent& event : events)
    {
        const ICFGNode* node = event.getEventInst();
        if (node == nullptr)
            continue;
        SaberTraceNode trace;
        trace.description = event.getEventDescription();
        trace.location = SaberSliceExportUtil::locFromICFG(node);
        if (node->getFun())
            trace.function = node->getFun()->getName();
        switch (event.getEventType())
        {
        case SVFBugEvent::SourceInst: trace.role = "source"; break;
        case SVFBugEvent::Free: trace.role = "free"; break;
        case SVFBugEvent::Use: trace.role = "use"; break;
        case SVFBugEvent::Branch: trace.role = "branch"; break;
        case SVFBugEvent::Caller:
        case SVFBugEvent::CallSite: trace.role = "call"; break;
        case SVFBugEvent::Loop:
        case SVFBugEvent::PotentialLoop: trace.role = "loop"; break;
        default: trace.role = "value_flow"; break;
        }
        trace.ir = canonicalIR(node->toString());
        if (!out.empty() && out.back().function == trace.function &&
                out.back().location.file == trace.location.file &&
                out.back().location.line == trace.location.line &&
                out.back().role == trace.role)
            continue;
        out.push_back(std::move(trace));
    }
}

void moveOriginsFirst(std::vector<SaberTraceNode>& trace)
{
    std::stable_partition(trace.begin(), trace.end(), [](const SaberTraceNode& node) {
        return node.role == "allocation" || node.role == "object_origin";
    });
}

std::string sha256(const std::string& text)
{
    static constexpr uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };
    std::vector<uint8_t> data(text.begin(), text.end());
    const uint64_t bitLength = static_cast<uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while ((data.size() % 64) != 56)
        data.push_back(0);
    for (int i = 7; i >= 0; --i)
        data.push_back(static_cast<uint8_t>(bitLength >> (i * 8)));
    std::array<uint32_t, 8> h = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    for (size_t offset = 0; offset < data.size(); offset += 64)
    {
        uint32_t w[64] = {};
        for (size_t i = 0; i < 16; ++i)
            w[i] = (uint32_t(data[offset + i * 4]) << 24) |
                   (uint32_t(data[offset + i * 4 + 1]) << 16) |
                   (uint32_t(data[offset + i * 4 + 2]) << 8) |
                   uint32_t(data[offset + i * 4 + 3]);
        for (size_t i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            const uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (size_t i = 0; i < 64; ++i)
        {
            const uint32_t s1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    std::ostringstream os;
    for (uint32_t word : h)
        os << std::hex << std::setw(8) << std::setfill('0') << word;
    return os.str();
}

std::string categoryDir(SaberSliceKind kind)
{
    switch (kind)
    {
    case SaberSliceKind::MEMORY_LEAK: return "memory_leak";
    case SaberSliceKind::DOUBLE_FREE: return "double_free";
    case SaberSliceKind::USE_AFTER_FREE: return "use_after_free";
    case SaberSliceKind::UNINIT_USE: return "uninit_use";
    }
    return "unknown";
}

std::string conditionJson(const std::vector<SaberPathEdge>& conditions,
                          const std::string& indent)
{
    std::ostringstream os;
    if (conditions.empty())
        return "{\"op\": \"true\"}";
    os << "{\"op\": \"and\", \"terms\": [";
    for (size_t i = 0; i < conditions.size(); ++i)
    {
        const bool truth = conditions[i].condition != "False";
        std::string file;
        int line = 0;
        int col = 0;
        SaberSliceExportUtil::parseLocFileLine(conditions[i].location, file, line);
        SaberSliceExportUtil::parseLocField(conditions[i].location, "cl", file, col);
        os << (i ? "," : "") << "\n" << indent
           << "{\"location\": {\"file\": \"" << jsonEscape(file)
           << "\", \"line\": " << line << ", \"column\": " << col
           << "}, \"value\": " << (truth ? "true" : "false") << "}";
    }
    os << "\n" << indent.substr(0, indent.size() >= 2 ? indent.size() - 2 : 0) << "]}";
    return os.str();
}

std::string preservedField(const std::filesystem::path& path, const char* field,
                           const char* fallback)
{
    std::ifstream in(path);
    if (!in)
        return fallback;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    cJSON* root = cJSON_Parse(buffer.str().c_str());
    if (!root)
        return fallback;
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, field);
    char* printed = item ? cJSON_PrintUnformatted(item) : nullptr;
    std::string result = printed ? printed : fallback;
    if (printed)
        cJSON_free(printed);
    cJSON_Delete(root);
    return result;
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
    os << i2 << "\"alert_id\": \"sha256:" << sha256(stableIdentity()) << "\",\n";
    os << i2 << "\"category\": \"" << saberSliceKindStr(kind) << "\",\n";

    auto writeNode = [&](const SaberTraceNode& n, const std::string& nodeInd) {
        os << "{ \"role\": \"" << jsonEscape(n.role)
           << "\", \"location\": { \"file\": \"" << jsonEscape(n.location.file)
           << "\", \"line\": " << n.location.line << ", \"column\": " << n.location.col
           << " }, \"function\": \"" << jsonEscape(n.function)
           << "\", \"node_kind\": \"ICFG\", \"ir\": \"" << jsonEscape(n.ir) << "\"";
        if (n.description == "True" || n.description == "False")
            os << ", \"condition\": " << (n.description == "True" ? "true" : "false");
        if (!n.sourceContext.empty())
            os << ", \"source_context\": { \"start_line\": " << n.contextStartLine
               << ", \"end_line\": " << n.contextEndLine << ", \"code\": \""
               << jsonEscape(n.sourceContext) << "\" }";
        os << " }";
        (void)nodeInd;
    };

    if (kind != SaberSliceKind::MEMORY_LEAK)
    {
        os << i2 << "\"path\": [";
        for (size_t k = 0; k < callTrace.size(); ++k)
        {
            os << (k ? "," : "") << "\n" << i3;
            writeNode(callTrace[k], i3);
        }
        os << (callTrace.empty() ? "" : ("\n" + i2)) << "],\n";
    }
    else
    {
        SaberTraceNode allocation;
        allocation.role = "allocation";
        allocation.location = {sourceFile, sourceLine, sourceCol};
        allocation.sourceContext = callTrace.empty() ? "" : callTrace.back().sourceContext;
        allocation.contextStartLine = sourceLine > 10 ? sourceLine - 10 : 1;
        allocation.contextEndLine = sourceLine + 3;
        os << i2 << "\"allocation\": ";
        writeNode(allocation, i2);
        os << ",\n" << i2 << "\"paths\": [";
        for (size_t p = 0; p < potentialFreeLocs.size(); ++p)
        {
            std::string f;
            int line = 0;
            SaberSliceExportUtil::parseLocFileLine(potentialFreeLocs[p], f, line);
            os << (p ? "," : "") << "\n" << i3
               << "{\"outcome\": \"freed\", \"condition\": "
               << conditionJson(p < safePathConditions.size()
                                ? safePathConditions[p] : pathConditions, i3 + "  ")
               << ", \"path\": [";
            if (!callTrace.empty())
            {
                os << "\n" << i3 << "  ";
                writeNode(callTrace.back(), i3 + "  ");
                os << ",";
            }
            SaberTraceNode freeNode;
            freeNode.role = "potential_free";
            freeNode.location = {f, line, 0};
            freeNode.contextStartLine = line > 10 ? line - 10 : 1;
            freeNode.contextEndLine = line + 3;
            if (p < potentialFreeContexts.size())
                freeNode.sourceContext = potentialFreeContexts[p];
            os << "\n" << i3 << "  ";
            writeNode(freeNode, i3 + "  ");
            os << "\n" << i3 << "]}";
        }
        os << (potentialFreeLocs.empty() ? "" : ("\n" + i2)) << "],\n";
        os << i2 << "\"leak_condition\": {\"op\": \"and\", \"terms\": ["
           << "{\"op\": \"allocation_reached\"}, {\"op\": \"not\", \"term\": "
           << "{\"op\": \"or\", \"terms\": [";
        for (size_t p = 0; p < potentialFreeLocs.size(); ++p)
            os << (p ? ", " : "")
               << conditionJson(p < safePathConditions.size()
                                ? safePathConditions[p] : pathConditions, i3);
        os << "]}}]";
        if (!bypassReturnLoc.empty())
            os << ", \"bypass_return\": \"" << jsonEscape(bypassReturnLoc) << "\"";
        os << "},\n";
    }

    os << i2 << "\"evidence\": { \"memory_object\": {"
       << "\"type\": \"" << jsonEscape(objectType) << "\", \"variable\": \""
       << jsonEscape(variable) << "\", \"allocator\": \"" << jsonEscape(allocator)
       << "\", \"descriptor\": \"" << jsonEscape(objectDescriptor)
       << "\", \"zeroing\": " << (zeroing ? "true" : "false")
       << "}, \"checker\": {\"report_kind\": \"" << jsonEscape(reportKind)
       << "\", \"leak_kind\": \"" << jsonEscape(leakKind)
       << "\", \"source_kind\": \"" << jsonEscape(sourceKind)
       << "\", \"path_truncated\": false} },\n";
    os << i2 << "\"classification\": null,\n";
    os << i2 << "\"reason\": \"\"\n";
    os << ind << "}";
    return os.str();
}

std::string SaberSlice::stableIdentity() const
{
    std::ostringstream os;
    os << saberSliceKindStr(kind) << "|" << sourceFile << ":" << sourceLine << ":" << sourceCol
       << "|" << freeFile << ":" << freeLine << "|" << free2File << ":" << free2Line
       << "|" << useFile << ":" << useLine << ":" << useCol << "|" << reportKind
       << "|" << leakKind << "|" << sourceKind << "|" << objectType << "|" << variable
       << "|" << objectDescriptor
       << "|" << sourceFunc << "|" << callerFile << ":" << callerLine;
    std::vector<std::string> stableNodes;
    for (const SaberTraceNode& node : callTrace)
    {
        std::ostringstream part;
        part << node.role << "@" << node.location.file << ":" << node.location.line
             << ":" << node.location.col << "#" << node.function << "#" << node.ir
             << "#" << node.description;
        stableNodes.push_back(part.str());
    }
    std::sort(stableNodes.begin(), stableNodes.end());
    for (const std::string& node : stableNodes)
        os << "|node:" << node;
    std::vector<std::string> stableFreeLocs = potentialFreeLocs;
    std::sort(stableFreeLocs.begin(), stableFreeLocs.end());
    for (const std::string& loc : stableFreeLocs)
        os << "|safe:" << loc;
    return os.str();
}

bool SaberSliceExportUtil::parseLocField(const std::string& locJson, const char* key,
                                         std::string& outStr, int& outInt)
{
    // Some ICFG nodes expose a decorated location such as
    // `CallICFGNode: { "ln": 12, ... }`, while others expose the JSON object
    // directly. Accept both forms so report export never loses call/free sites.
    std::string json = locJson;
    const size_t begin = json.find('{');
    const size_t end = json.rfind('}');
    if (begin != std::string::npos && end != std::string::npos && end >= begin)
        json = json.substr(begin, end - begin + 1);
    cJSON* lj = cJSON_Parse(json.c_str());
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

        // Debug paths commonly contain "source/<project>/..." while the configured
        // root already points at <project>. Resolve the suffix after the root basename.
        std::filesystem::path rootPath(root);
        const std::string marker = rootPath.filename().string() + "/";
        const size_t markerPos = file.find(marker);
        if (!marker.empty() && markerPos != std::string::npos)
            candidates.push_back((rootPath / file.substr(markerPos + marker.size())).string());
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

void SaberSliceCollector::addSourceContext(std::vector<SaberTraceNode>& trace) const
{
    for (SaberTraceNode& node : trace)
    {
        if (node.location.line <= 0 || node.location.file.empty())
            continue;
        node.contextStartLine = node.location.line > 10 ? node.location.line - 10 : 1;
        node.contextEndLine = node.location.line + 3;
        node.sourceContext = readCodeSnippet(node.location.file, node.location.line);
    }
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
    collectTrace(eventStack, out.callTrace);
    for (SaberTraceNode& node : out.callTrace)
        if (node.role == "source")
            node.role = (node.location.file == out.freeFile && node.location.line == out.freeLine)
                        ? "object_origin" : "allocation";
    addSourceContext(out.callTrace);

    std::ostringstream id;
    id << out.freeFile << ":" << out.freeLine << "->" << out.useFile << ":" << out.useLine;
    out.id = id.str();

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
    collectTrace(eventStack, out.callTrace);
    for (SaberTraceNode& node : out.callTrace)
        if (node.role == "source")
            node.role = "object_origin";
    addSourceContext(out.callTrace);

    // Identify by declaration site + variable so funnel-shared sinks stay distinct.
    if (!out.sourceFile.empty())
        out.id = "UNINIT@" + out.sourceFile + ":" + std::to_string(out.sourceLine) +
                 (out.variable.empty() ? "" : ("#" + out.variable)) +
                 "->" + out.useFile + ":" + std::to_string(out.useLine);
    else
        out.id = "UNINIT@" + out.useFile + ":" + std::to_string(out.useLine) + ":" +
                 std::to_string(out.useCol);
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
    collectTrace(eventStack, out.callTrace);
    for (SaberTraceNode& node : out.callTrace)
        if (node.role == "source")
            node.role = "allocation";
    addSourceContext(out.callTrace);

    out.id = "LEAK@" + out.leakKind + "@" + out.sourceFile + ":" +
             std::to_string(out.sourceLine);
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
    collectTrace(eventStack, out.callTrace);
    for (SaberTraceNode& node : out.callTrace)
        if (node.role == "source")
            node.role = "allocation";
    addSourceContext(out.callTrace);

    const std::string anchorFile = !out.freeFile.empty() ? out.freeFile : out.sourceFile;
    const int anchorLine = !out.freeFile.empty() ? out.freeLine : out.sourceLine;
    out.id = "DFREE@" + anchorFile + ":" + std::to_string(anchorLine);
    if (!out.free2File.empty())
        out.id += "->" + out.free2File + ":" + std::to_string(out.free2Line);

    return !anchorFile.empty() || anchorLine > 0;
}

bool SaberSliceCollector::collectSliceForPending(const SaberPendingReport& pending,
                                                 SaberSlice& out) const
{
    bool collected = false;
    switch (pending.bugType)
    {
    case GenericBug::USEAFTERFREE:
        collected = collectUAFSlice(pending.eventStack, pending.uafReportKind, out);
        out.objectDescriptor = pending.uafObjectDescriptor;
        if (collected && !out.objectDescriptor.empty())
        {
            const std::string objectLoc =
                out.objectDescriptor.substr(0, out.objectDescriptor.find('|'));
            SaberTraceNode allocation;
            allocation.role = "allocation";
            SaberSliceExportUtil::parseLocFileLine(
                objectLoc, allocation.location.file, allocation.location.line);
            if (!allocation.location.file.empty() && allocation.location.line > 0)
            {
                allocation.contextStartLine =
                    allocation.location.line > 10 ? allocation.location.line - 10 : 1;
                allocation.contextEndLine = allocation.location.line + 3;
                allocation.sourceContext =
                    readCodeSnippet(allocation.location.file, allocation.location.line);
                out.callTrace.insert(out.callTrace.begin(), std::move(allocation));
            }
        }
        break;
    case GenericBug::UNINIT:
        collected = collectUninitSlice(pending.uninitSource, pending.eventStack,
                                       pending.uninitSourceKind, pending.uninitAllocator,
                                       pending.uninitZeroing, out);
        break;
    case GenericBug::DOUBLEFREE:
        collected = collectDoubleFreeSlice(pending.eventStack, out);
        break;
    case GenericBug::NEVERFREE:
    case GenericBug::PARTIALLEAK:
        collected = collectLeakSlice(pending.bugType, pending.eventStack, out);
        break;
    default:
        return false;
    }
    if (collected)
    {
        if (pending.bugType != GenericBug::NEVERFREE)
        {
            out.potentialFreeLocs = pending.sinkLocs;
            for (const std::string& loc : pending.sinkLocs)
            {
                std::string file;
                int line = 0;
                SaberSliceExportUtil::parseLocFileLine(loc, file, line);
                out.potentialFreeContexts.push_back(readCodeSnippet(file, line));
            }
        }
        for (const GenericBug::EventStack& events : pending.sinkPathEvents)
        {
            std::vector<SaberPathEdge> conditions;
            for (const SVFBugEvent& event : events)
                conditions.push_back({event.getEventLoc(), event.getEventDescription()});
            out.safePathConditions.push_back(std::move(conditions));
        }
        out.bypassReturnLoc = pending.sinkBypassReturnLoc;
        if (pending.bugType == GenericBug::DOUBLEFREE)
        {
            if (!pending.sinkLocs.empty())
                SaberSliceExportUtil::parseLocFileLine(pending.sinkLocs[0],
                                                       out.freeFile, out.freeLine);
            if (pending.sinkLocs.size() > 1)
                SaberSliceExportUtil::parseLocFileLine(pending.sinkLocs[1],
                                                       out.free2File, out.free2Line);
            for (size_t i = 0; i < pending.sinkLocs.size() && i < 2; ++i)
            {
                SaberTraceNode node;
                node.role = i == 0 ? "first_free" : "second_free";
                SaberSliceExportUtil::parseLocFileLine(
                    pending.sinkLocs[i], node.location.file, node.location.line);
                node.contextStartLine = node.location.line > 10 ? node.location.line - 10 : 1;
                node.contextEndLine = node.location.line + 3;
                node.sourceContext = readCodeSnippet(node.location.file, node.location.line);
                out.callTrace.push_back(std::move(node));
            }
        }
        if (out.kind != SaberSliceKind::MEMORY_LEAK)
            moveOriginsFirst(out.callTrace);
    }
    return collected;
}

bool SaberSliceCollector::writeAlerts(const char* generatedBy) const
{
    if (alertOutDir.empty())
        return false;

    std::map<SaberSliceKind, std::set<std::string>> currentFiles;
    const std::string producer = generatedBy ? generatedBy : "";
    if (producer.find("DoubleFree") != std::string::npos)
        currentFiles[SaberSliceKind::DOUBLE_FREE];
    else if (producer.find("UseAfterFree") != std::string::npos)
        currentFiles[SaberSliceKind::USE_AFTER_FREE];
    else if (producer.find("Uninit") != std::string::npos)
        currentFiles[SaberSliceKind::UNINIT_USE];
    else
        currentFiles[SaberSliceKind::MEMORY_LEAK];
    bool ok = true;
    for (const SaberSlice& alert : slices)
    {
        const std::filesystem::path directory =
            std::filesystem::path(alertOutDir) / categoryDir(alert.kind);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
        {
            SVFUtil::errs() << "[SaberAlert] cannot create " << directory.string()
                            << ": " << error.message() << "\n";
            ok = false;
            continue;
        }

        const std::string alertId = "sha256:" + sha256(alert.stableIdentity());
        const std::string filename = alertId.substr(7) + ".json";
        const std::filesystem::path path = directory / filename;
        currentFiles[alert.kind].insert(filename);

        std::string json = alert.toJson("");
        const std::string oldClassification = preservedField(path, "classification", "null");
        const std::string oldReason = preservedField(path, "reason", "\"\"");
        const std::string classificationNeedle = "\"classification\": null";
        const size_t classificationPos = json.find(classificationNeedle);
        if (classificationPos != std::string::npos)
            json.replace(classificationPos, classificationNeedle.size(),
                         "\"classification\": " + oldClassification);
        const std::string reasonNeedle = "\"reason\": \"\"";
        const size_t reasonPos = json.find(reasonNeedle);
        if (reasonPos != std::string::npos)
            json.replace(reasonPos, reasonNeedle.size(), "\"reason\": " + oldReason);
        json += "\n";

        const std::filesystem::path tmp = path.string() + ".tmp";
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
        {
            SVFUtil::errs() << "[SaberAlert] cannot write " << tmp.string() << "\n";
            ok = false;
            continue;
        }
        out << json;
        out.close();
        std::filesystem::rename(tmp, path, error);
        if (error)
        {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(tmp, path, error);
        }
        if (error)
        {
            SVFUtil::errs() << "[SaberAlert] cannot replace " << path.string()
                            << ": " << error.message() << "\n";
            ok = false;
        }
    }

    // Each saber invocation owns exactly one category. Remove stale files only
    // from categories emitted by this collector.
    for (const auto& entry : currentFiles)
    {
        const std::filesystem::path directory =
            std::filesystem::path(alertOutDir) / categoryDir(entry.first);
        std::error_code error;
        for (const auto& file : std::filesystem::directory_iterator(directory, error))
        {
            if (error)
                break;
            if (!file.is_regular_file() || file.path().extension() != ".json")
                continue;
            if (entry.second.find(file.path().filename().string()) == entry.second.end())
                std::filesystem::remove(file.path(), error);
        }
    }
    return ok;
}

} // namespace SVF

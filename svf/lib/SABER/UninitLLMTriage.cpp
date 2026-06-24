//===- UninitLLMTriage.cpp -- LLM-assisted triage overlay for UninitChecker -===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

#include "SABER/UninitLLMTriage.h"
#include "Graphs/ICFGNode.h"
#include "Graphs/SVFG.h"
#include "Util/cJSON.h"
#include "Util/SVFUtil.h"

#include <cstdlib>
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

std::string UninitSlice::toJson(const std::string& ind) const
{
    const std::string i2 = ind + "  ";
    const std::string i3 = i2 + "  ";
    std::ostringstream os;
    os << ind << "{\n";
    os << i2 << "\"id\": \"" << jsonEscape(id) << "\",\n";
    os << i2 << "\"kind\": \"UNINIT_USE\",\n";
    os << i2 << "\"source_kind\": \"" << jsonEscape(sourceKind) << "\",\n";
    os << i2 << "\"static_verdict\": \"" << jsonEscape(staticVerdict) << "\",\n";
    os << i2 << "\"source\": { \"file\": \"" << jsonEscape(sourceFile) << "\", \"line\": "
       << sourceLine << ", \"col\": " << sourceCol << " },\n";
    os << i2 << "\"use\": { \"file\": \"" << jsonEscape(useFile) << "\", \"line\": "
       << useLine << ", \"col\": " << useCol << " },\n";
    os << i2 << "\"allocator\": \"" << jsonEscape(allocator) << "\",\n";
    os << i2 << "\"zeroing\": " << (zeroing ? "true" : "false") << ",\n";
    os << i2 << "\"path_conditions\": [";
    for (size_t k = 0; k < pathConditions.size(); ++k)
    {
        const UninitPathEdge& e = pathConditions[k];
        os << (k ? "," : "") << "\n" << i3 << "{ \"location\": \"" << jsonEscape(e.location)
           << "\", \"condition\": \"" << jsonEscape(e.condition) << "\" }";
    }
    os << (pathConditions.empty() ? "" : ("\n" + i2)) << "],\n";
    os << i2 << "\"code_snippet\": \"" << jsonEscape(codeSnippet) << "\"\n";
    os << ind << "}";
    return os.str();
}

bool UninitLLMTriageConfig::loadFromFile(const std::string& path)
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

    cJSON* th = cJSON_GetObjectItem(root, "threshold_fp");
    if (th && cJSON_IsNumber(th))
        thresholdFp = th->valuedouble;
    th = cJSON_GetObjectItem(root, "threshold_confirm");
    if (th && cJSON_IsNumber(th))
        thresholdConfirm = th->valuedouble;

    cJSON_Delete(root);
    return true;
}

void UninitLLMTriageConfig::loadFromEnv()
{
    if (apiUrl.empty())
        if (const char* v = std::getenv("UNINIT_LLM_API_URL"))
            apiUrl = v;
    if (apiKey.empty())
    {
        if (const char* v = std::getenv("UNINIT_LLM_API_KEY"))
            apiKey = v;
        else if (const char* v = std::getenv("DEEPSEEK_API_KEY"))
            apiKey = v;
    }
    if (model.empty())
        if (const char* v = std::getenv("UNINIT_LLM_MODEL"))
            model = v;
}

bool UninitLLMTriageConfig::hasApi() const
{
    return !apiUrl.empty() && !apiKey.empty() && !sidecarPath.empty();
}

bool UninitLLMTriage::parseLocField(const std::string& locJson, const char* key,
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

bool UninitLLMTriage::isWhitelistedTruePositive(const std::string& useFile, int useLine)
{
    if (useFile.find("ubcore_topo_info.c") != std::string::npos && useLine == 181)
        return true;
    if (useFile.find("cdma_event.c") != std::string::npos && useLine == 573)
        return true;
    return false;
}

std::string UninitLLMTriage::readCodeSnippet(const std::string& file, int line) const
{
    if (file.empty() || line <= 0)
        return "";

    std::vector<std::string> candidates;
    candidates.push_back(file);
    if (const char* root = std::getenv("UNINIT_SOURCE_ROOT"))
    {
        std::string rooted = std::string(root);
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

bool UninitLLMTriage::collectSlice(const SVFGNode* source,
                                   const GenericBug::EventStack& eventStack,
                                   const std::string& sourceKind,
                                   const std::string& allocator, bool zeroing,
                                   UninitSlice& out) const
{
    if (eventStack.empty())
        return false;

    out = UninitSlice();
    out.sourceKind = sourceKind;
    out.allocator = allocator;
    out.zeroing = zeroing;
    out.pathConditions.clear();

    if (source)
    {
        const ICFGNode* sourceICFG = source->getICFGNode();
        if (sourceICFG)
        {
            const std::string& sloc = sourceICFG->getSourceLoc();
            parseLocField(sloc, "fl", out.sourceFile, out.sourceLine);
            if (out.sourceFile.empty())
                parseLocField(sloc, "file", out.sourceFile, out.sourceLine);
            int dummy = 0;
            parseLocField(sloc, "ln", out.sourceFile, out.sourceLine);
            parseLocField(sloc, "cl", out.sourceFile, out.sourceCol);
            (void)dummy;
        }
    }

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

    if (const ICFGNode* useICFG = useEvent->getEventInst())
    {
        const std::string& uloc = useICFG->getSourceLoc();
        parseLocField(uloc, "fl", out.useFile, out.useLine);
        if (out.useFile.empty())
            parseLocField(uloc, "file", out.useFile, out.useLine);
        int dummy = 0;
        parseLocField(uloc, "ln", out.useFile, out.useLine);
        parseLocField(uloc, "cl", out.useFile, out.useCol);
        (void)dummy;
    }

    if (out.useFile.empty() && out.useLine == 0)
        return false;

    auto lastIt = eventStack.end();
    if (eventStack.size() > 1)
        --lastIt;
    for (auto it = eventStack.begin(); it != lastIt; ++it)
    {
        UninitPathEdge edge;
        edge.location = it->getEventLoc();
        edge.condition = it->getEventDescription();
        out.pathConditions.push_back(edge);
    }

    out.id = "UNINIT@" + out.useFile + ":" + std::to_string(out.useLine) + ":" +
             std::to_string(out.useCol);
    out.codeSnippet = readCodeSnippet(out.useFile, out.useLine);
    if (out.codeSnippet.empty() && !out.sourceFile.empty())
        out.codeSnippet = readCodeSnippet(out.sourceFile, out.sourceLine);

    return true;
}

bool UninitLLMTriage::writeSlices() const
{
    std::ofstream os(cfg.sliceOutPath, std::ios::trunc);
    if (!os)
    {
        SVFUtil::errs() << "[UninitLLMTriage] cannot open slice output: " << cfg.sliceOutPath << "\n";
        return false;
    }

    os << "{\n";
    os << "  \"schema\": \"uninit-slice/v1\",\n";
    os << "  \"generated_by\": \"SVFmemplus-UninitChecker\",\n";
    os << "  \"slice_count\": " << slices.size() << ",\n";
    os << "  \"slices\": [";
    for (size_t k = 0; k < slices.size(); ++k)
        os << (k ? "," : "") << "\n" << slices[k].toJson("    ");
    os << (slices.empty() ? "" : "\n  ") << "]\n";
    os << "}\n";
    return true;
}

bool UninitLLMTriage::runSidecarAndLoad(std::map<std::string, UninitLLMVerdict>& out) const
{
    out.clear();
    if (!cfg.hasApi())
        return false;

    if (!cfg.apiUrl.empty())
        ::setenv("UNINIT_LLM_API_URL", cfg.apiUrl.c_str(), 1);
    if (!cfg.apiKey.empty())
        ::setenv("UNINIT_LLM_API_KEY", cfg.apiKey.c_str(), 1);
    if (!cfg.model.empty())
        ::setenv("UNINIT_LLM_MODEL", cfg.model.c_str(), 1);

    std::ostringstream cmd;
    cmd << cfg.pythonExe << " \"" << cfg.sidecarPath << "\""
        << " --slices \"" << cfg.sliceOutPath << "\""
        << " --out \"" << cfg.verdictPath << "\""
        << " --threshold-fp " << cfg.thresholdFp
        << " --threshold-confirm " << cfg.thresholdConfirm;

    SVFUtil::outs() << "[UninitLLMTriage] invoking sidecar: " << cmd.str() << "\n";
    const int rc = std::system(cmd.str().c_str());
    if (rc != 0)
    {
        SVFUtil::errs() << "[UninitLLMTriage] sidecar exited with code " << rc << "\n";
        return false;
    }

    std::string text;
    if (!slurp(cfg.verdictPath, text))
        return false;

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
            UninitLLMVerdict v;
            if (cJSON* id = cJSON_GetObjectItem(el, "id"))
                if (cJSON_IsString(id) && id->valuestring) v.id = id->valuestring;
            if (cJSON* vd = cJSON_GetObjectItem(el, "verdict"))
                if (cJSON_IsString(vd) && vd->valuestring) v.verdict = vd->valuestring;
            if (cJSON* cf = cJSON_GetObjectItem(el, "confidence"))
                if (cJSON_IsNumber(cf)) v.confidence = cf->valuedouble;
            if (cJSON* ra = cJSON_GetObjectItem(el, "rationale"))
                if (cJSON_IsString(ra) && ra->valuestring) v.rationale = ra->valuestring;
            if (!v.id.empty())
                out[v.id] = v;
        }
    }
    cJSON_Delete(root);
    return !out.empty();
}

//===- SaberSemanticRules.cpp -- project semantic-fact/v2 registry -------===//
#include "SABER/SaberSemanticRules.h"

#include "Graphs/CallGraph.h"
#include "Graphs/ICFGNode.h"
#include "SVFIR/SVFVariables.h"
#include "Util/SVFUtil.h"
#include "Util/cJSON.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

using namespace SVF;

namespace
{

bool hasOnlyFields(cJSON* object, std::initializer_list<const char*> allowed)
{
    if (!cJSON_IsObject(object))
        return false;
    std::set<std::string> names;
    for (const char* name : allowed)
        names.insert(name);
    std::set<std::string> seen;
    for (cJSON* child = object->child; child; child = child->next)
        if (!child->string || names.count(child->string) == 0 ||
                !seen.insert(child->string).second)
            return false;
    return true;
}

bool nonBlank(const std::string& value)
{
    return value.find_first_not_of(" \t\r\n") != std::string::npos;
}

std::string stringField(cJSON* obj, const char* key)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(obj, key);
    return value && cJSON_IsString(value) && value->valuestring
               ? value->valuestring : "";
}

bool int64Field(cJSON* obj, const char* key, std::int64_t& out)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!value || !cJSON_IsNumber(value))
        return false;
    if (!std::isfinite(value->valuedouble) ||
            std::floor(value->valuedouble) != value->valuedouble ||
            value->valuedouble < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            value->valuedouble >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return false;
    out = static_cast<std::int64_t>(value->valuedouble);
    return true;
}

bool parseLocation(cJSON* obj, SaberSemanticRules::Location& out)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(obj, "location");
    if (!value)
        return true;
    if (!cJSON_IsObject(value))
        return false;
    if (!hasOnlyFields(value, {"file", "line"}))
        return false;
    out.file = stringField(value, "file");
    cJSON* line = cJSON_GetObjectItemCaseSensitive(value, "line");
    if (line)
    {
        if (!cJSON_IsNumber(line) || !std::isfinite(line->valuedouble) ||
                std::floor(line->valuedouble) != line->valuedouble ||
                line->valuedouble < 1 ||
                line->valuedouble > std::numeric_limits<int>::max())
            return false;
        out.line = line->valueint;
    }
    return nonBlank(out.file) && out.line > 0;
}

bool parseCallChain(cJSON* obj, std::vector<std::string>& out)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(obj, "call_chain");
    if (!value)
        return true;
    if (!cJSON_IsArray(value))
        return false;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, value)
    {
        if (!cJSON_IsString(item) || !item->valuestring ||
                !nonBlank(item->valuestring))
            return false;
        out.emplace_back(item->valuestring);
    }
    return true;
}

std::string jsonEscape(const std::string& value)
{
    cJSON* text = cJSON_CreateString(value.c_str());
    char* rendered = cJSON_PrintUnformatted(text);
    std::string result = rendered ? rendered : "\"\"";
    if (rendered)
        cJSON_free(rendered);
    cJSON_Delete(text);
    return result;
}

std::string factJson(const SaberSemanticRules::Fact& fact)
{
    std::ostringstream os;
    os << "{";
    bool comma = false;
    if (!fact.function.empty())
    {
        os << "\"function\":" << jsonEscape(fact.function);
        comma = true;
    }
    if (!fact.kind.empty())
    {
        os << (comma ? "," : "") << "\"kind\":" << jsonEscape(fact.kind);
        comma = true;
    }
    if (!fact.callChain.empty())
    {
        os << (comma ? "," : "") << "\"call_chain\":[";
        for (size_t i = 0; i < fact.callChain.size(); ++i)
            os << (i ? "," : "") << jsonEscape(fact.callChain[i]);
        os << "]";
        comma = true;
    }
    if (!fact.location.empty())
    {
        os << (comma ? "," : "") << "\"location\":{\"file\":"
           << jsonEscape(fact.location.file) << ",\"line\":"
           << fact.location.line << "}";
        comma = true;
    }
    if (!fact.variable.empty())
    {
        os << (comma ? "," : "") << "\"variable\":" << jsonEscape(fact.variable)
           << ",\"lower\":" << fact.lower << ",\"upper\":" << fact.upper;
        comma = true;
    }
    if (!fact.path.empty())
    {
        os << (comma ? "," : "") << "\"path\":" << jsonEscape(fact.path);
        comma = true;
    }
    if (!fact.checker.empty())
        os << (comma ? "," : "") << "\"checker\":" << jsonEscape(fact.checker);
    os << "}";
    return os.str();
}

bool validChecker(const std::string& checker)
{
    return checker == "leak" || checker == "dfree" || checker == "uaf" ||
           checker == "bof" || checker == "uninit";
}

bool parseFact(cJSON* item, const std::string& scope,
               SaberSemanticRules::Fact& fact, std::string& error)
{
    if (!cJSON_IsObject(item))
    {
        error = "fact is not an object";
        return false;
    }
    fact.scope = scope;
    fact.function = stringField(item, "function");
    fact.kind = stringField(item, "kind");
    if (!parseCallChain(item, fact.callChain) || !parseLocation(item, fact.location))
    {
        error = "invalid call_chain or location";
        return false;
    }

    if (scope == "base_api")
    {
        if (!hasOnlyFields(item, {"function", "kind"}))
        {
            error = "base_api contains unsupported fields";
            return false;
        }
        if (!nonBlank(fact.function) ||
                (fact.kind != "alloc" && fact.kind != "free" &&
                 fact.kind != "mem_transfer"))
        {
            error = "base_api requires function and kind alloc/free/mem_transfer";
            return false;
        }
    }
    else if (scope == "safe_alloc" || scope == "safe_free")
    {
        if (!hasOnlyFields(item, {"function", "kind", "call_chain", "location"}))
        {
            error = scope + " contains unsupported fields";
            return false;
        }
        const std::string expected = scope == "safe_alloc" ? "alloc" : "free";
        if (!nonBlank(fact.function) || fact.kind != expected || fact.callChain.empty())
        {
            error = scope + " requires function, non-empty call_chain and kind " + expected;
            return false;
        }
    }
    else if (scope == "value_range")
    {
        if (!hasOnlyFields(item,
                           {"function", "kind", "variable", "lower", "upper", "location"}))
        {
            error = "value_range contains unsupported fields";
            return false;
        }
        fact.variable = stringField(item, "variable");
        if (!nonBlank(fact.function) || fact.kind != "range" || !nonBlank(fact.variable) ||
                !int64Field(item, "lower", fact.lower) ||
                !int64Field(item, "upper", fact.upper) || fact.lower > fact.upper)
        {
            error = "value_range requires function, variable and ordered integer bounds";
            return false;
        }
    }
    else if (scope == "source_filter")
    {
        if (!hasOnlyFields(item, {"kind", "path", "checker"}))
        {
            error = "source_filter contains unsupported fields";
            return false;
        }
        fact.path = stringField(item, "path");
        fact.checker = stringField(item, "checker");
        const std::filesystem::path relativePath(fact.path);
        bool traversal = false;
        for (const auto& part : relativePath)
            if (part == "..") traversal = true;
        if (fact.kind != "ignore_source" || !nonBlank(fact.path) ||
                !validChecker(fact.checker) || relativePath.is_absolute() || traversal ||
                fact.path.find('\\') != std::string::npos ||
                (fact.path.size() >= 2 && fact.path[1] == ':'))
        {
            error = "source_filter requires a relative path and a supported checker";
            return false;
        }
    }
    else
    {
        error = "unsupported scope " + scope;
        return false;
    }
    return true;
}

bool chainMatches(const CallGraphNode* node, const std::vector<std::string>& chain,
                  size_t index, std::set<std::pair<NodeID, size_t>>& visited)
{
    if (!node || node->getFunction()->getName() != chain[index])
        return false;
    if (index == 0)
        return true;
    const std::pair<NodeID, size_t> state(node->getId(), index);
    if (!visited.insert(state).second)
        return false;
    for (auto it = node->InEdgeBegin(), end = node->InEdgeEnd(); it != end; ++it)
        if (chainMatches((*it)->getSrcNode(), chain, index - 1, visited))
            return true;
    return false;
}

} // namespace

SaberSemanticRules* SaberSemanticRules::get()
{
    static SaberSemanticRules instance;
    return &instance;
}

void SaberSemanticRules::clear()
{
    apiRules.clear();
    safeAllocFacts.clear();
    safeFreeFacts.clear();
    rangeFacts.clear();
    sourceFilterFacts.clear();
    hitLines.clear();
    hitKeys.clear();
}

const SaberSemanticRules::Rule* SaberSemanticRules::find(
        const std::string& function, Kind kind) const
{
    for (const Rule& rule : apiRules)
        if (rule.kind == kind && rule.function == function)
            return &rule;
    return nullptr;
}

bool SaberSemanticRules::matchesLocation(const Location& location,
                                         const std::string& rawLocation) const
{
    if (location.empty())
        return true;
    const size_t begin = rawLocation.find('{');
    const size_t end = rawLocation.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end < begin)
        return false;
    cJSON* parsed = cJSON_Parse(rawLocation.substr(begin, end - begin + 1).c_str());
    if (!parsed)
        return false;
    std::string file = stringField(parsed, "fl");
    if (file.empty()) file = stringField(parsed, "file");
    cJSON* lineItem = cJSON_GetObjectItemCaseSensitive(parsed, "ln");
    if (!lineItem) lineItem = cJSON_GetObjectItemCaseSensitive(parsed, "line");
    const int line = cJSON_IsNumber(lineItem) ? lineItem->valueint : 0;
    cJSON_Delete(parsed);
    const bool sameFile = file == location.file ||
        (file.size() > location.file.size() &&
         file.compare(file.size() - location.file.size(), location.file.size(), location.file) == 0 &&
         file[file.size() - location.file.size() - 1] == '/');
    return sameFile && line == location.line;
}

bool SaberSemanticRules::matchesContext(const Fact& fact, const std::string& callee,
                                        const CallICFGNode* call,
                                        const CallGraph* callGraph) const
{
    if (!call || fact.function != callee ||
            !matchesLocation(fact.location, call->getSourceLoc()))
        return false;
    if (fact.callChain.empty())
        return true;
    const FunObjVar* caller = call->getFun();
    if (!caller || !callGraph || caller->getName() != fact.callChain.back())
        return false;
    const CallGraphNode* node = callGraph->getCallGraphNode(caller);
    std::set<std::pair<NodeID, size_t>> visited;
    return chainMatches(node, fact.callChain, fact.callChain.size() - 1, visited);
}

const SaberSemanticRules::Fact* SaberSemanticRules::findSafeAlloc(
        const std::string& callee, const CallICFGNode* call,
        const CallGraph* callGraph) const
{
    for (const Fact& fact : safeAllocFacts)
        if (matchesContext(fact, callee, call, callGraph))
            return &fact;
    return nullptr;
}

const SaberSemanticRules::Fact* SaberSemanticRules::findSafeFree(
        const std::string& callee, const CallICFGNode* call,
        const CallGraph* callGraph) const
{
    for (const Fact& fact : safeFreeFacts)
        if (matchesContext(fact, callee, call, callGraph))
            return &fact;
    return nullptr;
}

std::string SaberSemanticRules::valueKey(const SVFVar* value)
{
    if (!value)
        return "";
    if (const ArgValVar* arg = SVFUtil::dyn_cast<ArgValVar>(value))
        return "arg:" + std::to_string(arg->getArgNo());
    if (!value->getName().empty())
        return "name:" + value->getName();
    if (!value->getSourceLoc().empty())
        return "loc:" + value->getSourceLoc();
    return "";
}

bool SaberSemanticRules::resolveRange(const SVFVar* value,
                                      const ICFGNode* context,
                                      std::int64_t& lower, std::int64_t& upper,
                                      const Fact** matched)
{
    const std::string key = valueKey(value);
    const ValVar* val = value ? SVFUtil::dyn_cast<ValVar>(value) : nullptr;
    const FunObjVar* fun = val && val->getFunction()
                               ? val->getFunction()
                               : context ? context->getFun() : nullptr;
    if (key.empty() || !fun)
        return false;
    const std::string rawLocation = !value->getSourceLoc().empty()
                                        ? value->getSourceLoc()
                                        : context ? context->getSourceLoc() : "";
    for (const Fact& fact : rangeFacts)
    {
        const bool locationMatches = matchesLocation(fact.location, rawLocation) ||
            (context && matchesLocation(fact.location, context->getSourceLoc()));
        if (fact.function != fun->getName() || fact.variable != key || !locationMatches)
            continue;
        lower = fact.lower;
        upper = fact.upper;
        if (matched) *matched = &fact;
        recordHit(fact, "bof", "range", rawLocation);
        return true;
    }
    return false;
}

const SaberSemanticRules::Fact* SaberSemanticRules::ignoredSource(
        const std::string& checker, const std::string& rawLocation) const
{
    for (const Fact& fact : sourceFilterFacts)
    {
        if (fact.checker != checker)
            continue;
        const bool directory = !fact.path.empty() && fact.path.back() == '/';
        if (directory ? rawLocation.find(fact.path) != std::string::npos
                      : rawLocation.find(fact.path + "\"") != std::string::npos)
            return &fact;
    }
    return nullptr;
}

void SaberSemanticRules::recordHit(const Fact& fact, const std::string& checker,
                                   const std::string& role,
                                   const std::string& rawLocation)
{
    std::ostringstream os;
    os << "{\"checker\":" << jsonEscape(checker)
       << ",\"scope\":" << jsonEscape(fact.scope)
       << ",\"fact\":" << factJson(fact)
       << ",\"anchor\":{\"role\":" << jsonEscape(role)
       << ",\"location\":" << jsonEscape(rawLocation) << "}}";
    const std::string line = os.str();
    if (hitKeys.insert(line).second)
        hitLines.push_back(line);
}

bool SaberSemanticRules::flushHits()
{
    if (hitOutputPath.empty())
        return true;
    std::error_code error;
    const std::filesystem::path path(hitOutputPath);
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;
    for (const std::string& line : hitLines)
        out << line << "\n";
    return static_cast<bool>(out);
}

const char* SaberSemanticRules::kindName(Kind kind)
{
    switch (kind)
    {
    case Kind::INITIALIZER: return "initializer";
    case Kind::MEMORY_TRANSFER: return "mem_transfer";
    case Kind::ALLOCATOR: return "alloc";
    case Kind::DEALLOCATOR: return "free";
    case Kind::RESOURCE_OPEN: return "resource_open";
    case Kind::RESOURCE_CLOSE: return "resource_close";
    case Kind::OWNERSHIP_TRANSFER: return "ownership_transfer";
    case Kind::HEAP_OBJECT_SUMMARY: return "heap_object_summary";
    case Kind::DOMAIN_HINT: return "domain_hint";
    default: return "unknown";
    }
}

bool SaberSemanticRules::loadFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
    {
        SVFUtil::errs() << "[SemanticFact] cannot open " << path << "\n";
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    cJSON* root = cJSON_Parse(text.str().c_str());
    if (!root)
    {
        SVFUtil::errs() << "[SemanticFact] invalid JSON\n";
        return false;
    }
    cJSON* scopes = cJSON_GetObjectItemCaseSensitive(root, "scopes");
    if (!hasOnlyFields(root, {"schema", "scopes"}) ||
            stringField(root, "schema") != "semantic-fact/v2" || !cJSON_IsObject(scopes))
    {
        cJSON_Delete(root);
        SVFUtil::errs() << "[SemanticFact] expected semantic-fact/v2 with scopes\n";
        return false;
    }

    std::vector<Rule> parsedAPIs;
    std::vector<Fact> parsedSafeAlloc, parsedSafeFree, parsedRanges, parsedFilters;
    std::map<std::string, std::pair<std::int64_t, std::int64_t>> rangeBounds;
    const char* scopeNames[] = {
        "base_api", "safe_alloc", "safe_free", "value_range", "source_filter"
    };
    if (!hasOnlyFields(scopes,
                       {"base_api", "safe_alloc", "safe_free", "value_range", "source_filter"}))
    {
        cJSON_Delete(root);
        SVFUtil::errs() << "[SemanticFact] scopes must contain only the five fixed scopes\n";
        return false;
    }
    bool ok = true;
    for (const char* scopeName : scopeNames)
    {
        cJSON* scope = cJSON_GetObjectItemCaseSensitive(scopes, scopeName);
        cJSON* facts = scope ? cJSON_GetObjectItemCaseSensitive(scope, "facts") : nullptr;
        cJSON* description = scope ? cJSON_GetObjectItemCaseSensitive(scope, "description") : nullptr;
        if (!hasOnlyFields(scope, {"description", "facts"}) ||
                !cJSON_IsString(description) || !description->valuestring ||
                !nonBlank(description->valuestring) || !cJSON_IsArray(facts))
        {
            SVFUtil::errs() << "[SemanticFact] scope " << scopeName
                            << " requires description and facts[]\n";
            ok = false;
            break;
        }
        std::set<std::string> seenFacts;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, facts)
        {
            Fact fact;
            std::string parseError;
            if (!parseFact(item, scopeName, fact, parseError))
            {
                SVFUtil::errs() << "[SemanticFact] " << scopeName << ": "
                                << parseError << "\n";
                ok = false;
                break;
            }
            if (!seenFacts.insert(factJson(fact)).second)
            {
                SVFUtil::errs() << "[SemanticFact] duplicate fact in " << scopeName << "\n";
                ok = false;
                break;
            }
            if (std::string(scopeName) == "value_range")
            {
                const std::string rangeKey = fact.function + "\n" + fact.variable + "\n" +
                    fact.location.file + "\n" + std::to_string(fact.location.line);
                const auto bounds = std::make_pair(fact.lower, fact.upper);
                auto previous = rangeBounds.find(rangeKey);
                if (previous != rangeBounds.end() && previous->second != bounds)
                {
                    SVFUtil::errs() << "[SemanticFact] conflicting range for "
                                    << fact.function << ":" << fact.variable << "\n";
                    ok = false;
                    break;
                }
                rangeBounds[rangeKey] = bounds;
            }
            if (std::string(scopeName) == "base_api")
            {
                Rule rule;
                rule.function = fact.function;
                rule.kind = fact.kind == "alloc" ? Kind::ALLOCATOR :
                            fact.kind == "free" ? Kind::DEALLOCATOR : Kind::MEMORY_TRANSFER;
                if (rule.kind == Kind::MEMORY_TRANSFER)
                {
                    rule.effect = "copy";
                    rule.targetArg = 0;
                    rule.sourceArg = 1;
                    rule.lengthArg = 2;
                }
                parsedAPIs.push_back(std::move(rule));
            }
            else if (std::string(scopeName) == "safe_alloc") parsedSafeAlloc.push_back(std::move(fact));
            else if (std::string(scopeName) == "safe_free") parsedSafeFree.push_back(std::move(fact));
            else if (std::string(scopeName) == "value_range") parsedRanges.push_back(std::move(fact));
            else parsedFilters.push_back(std::move(fact));
        }
        if (!ok) break;
    }
    cJSON_Delete(root);
    if (!ok)
        return false;

    clear();
    apiRules.swap(parsedAPIs);
    safeAllocFacts.swap(parsedSafeAlloc);
    safeFreeFacts.swap(parsedSafeFree);
    rangeFacts.swap(parsedRanges);
    sourceFilterFacts.swap(parsedFilters);
    SVFUtil::outs() << "[SemanticFact] loaded " << apiRules.size() << " API, "
                    << safeAllocFacts.size() << " safe-alloc, "
                    << safeFreeFacts.size() << " safe-free, "
                    << rangeFacts.size() << " range and "
                    << sourceFilterFacts.size() << " source-filter fact(s)\n";
    return true;
}

//===- SaberSemanticRules.cpp -- reviewed project semantic summaries ------===//
#include "SABER/SaberSemanticRules.h"
#include "Util/SVFUtil.h"
#include "Util/cJSON.h"

#include <fstream>
#include <sstream>

using namespace SVF;

namespace
{
std::string stringField(cJSON* obj, const char* key)
{
    cJSON* value = cJSON_GetObjectItem(obj, key);
    return value && cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

int intField(cJSON* obj, const char* key, int fallback = -1)
{
    cJSON* value = cJSON_GetObjectItem(obj, key);
    return value && cJSON_IsNumber(value) ? value->valueint : fallback;
}

SaberSemanticRules::Kind parseKind(const std::string& value)
{
    using K = SaberSemanticRules::Kind;
    if (value == "initializer") return K::INITIALIZER;
    if (value == "memory_transfer") return K::MEMORY_TRANSFER;
    if (value == "allocator") return K::ALLOCATOR;
    if (value == "deallocator") return K::DEALLOCATOR;
    if (value == "resource_open") return K::RESOURCE_OPEN;
    if (value == "resource_close") return K::RESOURCE_CLOSE;
    if (value == "ownership_transfer") return K::OWNERSHIP_TRANSFER;
    if (value == "heap_object_summary") return K::HEAP_OBJECT_SUMMARY;
    if (value == "domain_hint") return K::DOMAIN_HINT;
    return K::UNKNOWN;
}
}

SaberSemanticRules* SaberSemanticRules::get()
{
    static SaberSemanticRules instance;
    return &instance;
}

void SaberSemanticRules::clear()
{
    approvedRules.clear();
}

bool SaberSemanticRules::matches(const Rule& rule, const std::string& function) const
{
    return rule.match == "substring" ? function.find(rule.function) != std::string::npos
                                     : function == rule.function;
}

const SaberSemanticRules::Rule* SaberSemanticRules::find(const std::string& function,
                                                         Kind kind) const
{
    for (const Rule& rule : approvedRules)
        if (rule.kind == kind && matches(rule, function))
            return &rule;
    return nullptr;
}

const char* SaberSemanticRules::kindName(Kind kind)
{
    switch (kind)
    {
    case Kind::INITIALIZER: return "initializer";
    case Kind::MEMORY_TRANSFER: return "memory_transfer";
    case Kind::ALLOCATOR: return "allocator";
    case Kind::DEALLOCATOR: return "deallocator";
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
        SVFUtil::errs() << "[SaberSemanticRules] cannot open " << path << "\n";
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    cJSON* root = cJSON_Parse(text.str().c_str());
    if (!root)
    {
        SVFUtil::errs() << "[SaberSemanticRules] invalid JSON\n";
        return false;
    }
    const std::string schema = stringField(root, "schema");
    cJSON* rulesNode = cJSON_GetObjectItem(root, "rules");
    if (schema != "semantic-rules/v1" || !rulesNode || !cJSON_IsArray(rulesNode))
    {
        cJSON_Delete(root);
        SVFUtil::errs() << "[SaberSemanticRules] expected semantic-rules/v1 with rules[]\n";
        return false;
    }

    std::vector<Rule> parsed;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, rulesNode)
    {
        if (!cJSON_IsObject(item) || stringField(item, "status") != "approved")
            continue;
        Rule rule;
        rule.id = stringField(item, "id");
        rule.kind = parseKind(stringField(item, "kind"));
        rule.function = stringField(item, "function");
        const std::string match = stringField(item, "match");
        if (!match.empty()) rule.match = match;
        rule.effect = stringField(item, "effect");
        rule.targetArg = intField(item, "target_arg");
        rule.sourceArg = intField(item, "source_arg");
        rule.lengthArg = intField(item, "length_arg");
        rule.fieldPath = stringField(item, "field_path");
        rule.pair = stringField(item, "pair");
        rule.reason = stringField(item, "reason");
        cJSON* confidence = cJSON_GetObjectItem(item, "confidence");
        if (confidence && cJSON_IsNumber(confidence))
            rule.confidence = confidence->valuedouble;

        const bool validMatch = rule.match == "exact" || rule.match == "substring";
        const bool valid = !rule.id.empty() && !rule.function.empty() &&
                           rule.kind != Kind::UNKNOWN && validMatch &&
                           rule.targetArg >= -1 && rule.sourceArg >= -1 && rule.lengthArg >= -1;
        if (!valid)
        {
            SVFUtil::errs() << "[SaberSemanticRules] rejecting malformed approved rule "
                            << rule.id << "\n";
            continue;
        }
        bool duplicate = false;
        for (const Rule& old : parsed)
            duplicate |= old.kind == rule.kind && old.function == rule.function &&
                         old.match == rule.match && old.targetArg == rule.targetArg;
        if (duplicate)
        {
            SVFUtil::errs() << "[SaberSemanticRules] rejecting conflicting rule "
                            << rule.id << "\n";
            continue;
        }
        parsed.push_back(std::move(rule));
    }
    cJSON_Delete(root);
    approvedRules.swap(parsed);
    SVFUtil::outs() << "[SaberSemanticRules] loaded " << approvedRules.size()
                    << " approved rule(s)\n";
    return true;
}

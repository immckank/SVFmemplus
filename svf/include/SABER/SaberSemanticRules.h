//===- SaberSemanticRules.h -- reviewed project semantic summaries --------===//
#ifndef SABER_SEMANTIC_RULES_H_
#define SABER_SEMANTIC_RULES_H_

#include <string>
#include <vector>

namespace SVF
{

class SaberSemanticRules
{
public:
    enum class Kind
    {
        INITIALIZER,
        MEMORY_TRANSFER,
        ALLOCATOR,
        DEALLOCATOR,
        RESOURCE_OPEN,
        RESOURCE_CLOSE,
        OWNERSHIP_TRANSFER,
        HEAP_OBJECT_SUMMARY,
        DOMAIN_HINT,
        UNKNOWN
    };

    struct Rule
    {
        std::string id;
        Kind kind = Kind::UNKNOWN;
        std::string function;
        std::string match = "exact";
        std::string effect;
        int targetArg = -1;
        int sourceArg = -1;
        int lengthArg = -1;
        std::string fieldPath;
        std::string pair;
        std::string reason;
        double confidence = 0.0;
    };

    static SaberSemanticRules* get();
    bool loadFile(const std::string& path);
    void clear();
    const std::vector<Rule>& rules() const { return approvedRules; }

    const Rule* find(const std::string& function, Kind kind) const;
    static const char* kindName(Kind kind);

private:
    bool matches(const Rule& rule, const std::string& function) const;
    std::vector<Rule> approvedRules;
};

} // namespace SVF
#endif

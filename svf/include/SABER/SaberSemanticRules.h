//===- SaberSemanticRules.h -- project semantic-fact/v2 registry ---------===//
#ifndef SABER_SEMANTIC_RULES_H_
#define SABER_SEMANTIC_RULES_H_

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace SVF
{

class CallGraph;
class CallICFGNode;
class ICFGNode;
class SVFVar;

/**
 * A deliberately small project semantic registry.
 *
 * semantic-fact/v2 has five fixed scopes.  Facts have no ids, revisions,
 * status, priority or timestamps; equality of their normalized content is
 * their identity.  The legacy Rule view is retained only as a compatibility
 * facade for SaberCheckerAPI, SaberInitAPI and SaberMemTransferAPI.
 */
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

    struct Location
    {
        std::string file;
        int line = 0;

        bool empty() const { return file.empty() && line == 0; }
    };

    struct Fact
    {
        std::string scope;
        std::string function;
        std::string kind;
        std::vector<std::string> callChain;
        Location location;
        std::string variable;
        std::int64_t lower = 0;
        std::int64_t upper = 0;
        std::string path;
        std::string checker;
    };

    /// Compatibility projection used by the existing API tables.
    struct Rule
    {
        Kind kind = Kind::UNKNOWN;
        std::string function;
        std::string effect;
        int targetArg = -1;
        int sourceArg = -1;
        int lengthArg = -1;
        std::string reason;
    };

    static SaberSemanticRules* get();

    /// Load one project semantic-fact/v2 file. Parsing is transactional.
    bool loadFile(const std::string& path);
    void clear();

    /// Configure the per-run lightweight semantic hit log.
    void setHitOutput(const std::string& path) { hitOutputPath = path; }
    bool flushHits();

    const std::vector<Rule>& rules() const { return apiRules; }
    const Rule* find(const std::string& function, Kind kind) const;

    const Fact* findSafeAlloc(const std::string& callee,
                              const CallICFGNode* call,
                              const CallGraph* callGraph) const;
    const Fact* findSafeFree(const std::string& callee,
                             const CallICFGNode* call,
                             const CallGraph* callGraph) const;

    /// Return a directly specified BOF interval for this value/program point.
    bool resolveRange(const SVFVar* value, const ICFGNode* context,
                      std::int64_t& lower, std::int64_t& upper,
                      const Fact** matched = nullptr);

    /// Checker-specific filtering of source/seed nodes, never final warnings.
    const Fact* ignoredSource(const std::string& checker,
                              const std::string& rawLocation) const;

    /// Record an analysis-time application. The complete fact is embedded.
    void recordHit(const Fact& fact, const std::string& checker,
                   const std::string& role, const std::string& rawLocation);

    static std::string valueKey(const SVFVar* value);
    static const char* kindName(Kind kind);

private:
    bool matchesContext(const Fact& fact, const std::string& callee,
                        const CallICFGNode* call,
                        const CallGraph* callGraph) const;
    bool matchesLocation(const Location& location,
                         const std::string& rawLocation) const;

    std::vector<Rule> apiRules;
    std::vector<Fact> safeAllocFacts;
    std::vector<Fact> safeFreeFacts;
    std::vector<Fact> rangeFacts;
    std::vector<Fact> sourceFilterFacts;
    std::vector<std::string> hitLines;
    std::set<std::string> hitKeys;
    std::string hitOutputPath;
};

} // namespace SVF
#endif

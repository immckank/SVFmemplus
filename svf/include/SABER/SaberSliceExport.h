//===- SaberSliceExport.h -- SABER alert context export for downstream LLM --===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

#ifndef SABER_SLICE_EXPORT_H_
#define SABER_SLICE_EXPORT_H_

#include "Util/SVFBugReport.h"
#include "Util/GeneralType.h"
#include "Graphs/SVFG.h"
#include <string>
#include <vector>

namespace SVF
{

class ICFGNode;

/// Bug category encoded in slice JSON for downstream analysis tools.
enum class SaberSliceKind
{
    USE_AFTER_FREE,
    UNINIT_USE,
    MEMORY_LEAK,
    DOUBLE_FREE
};

const char* saberSliceKindStr(SaberSliceKind kind);

struct SaberPathEdge
{
    std::string location;
    std::string condition;
};

struct SaberSourceLoc
{
    std::string file;
    int line = 0;
    int col = 0;

    bool empty() const
    {
        return file.empty() && line == 0;
    }
};

struct SaberTraceNode
{
    std::string role;
    std::string function;
    SaberSourceLoc location;
    std::string description;
    std::string ir;
    int contextStartLine = 0;
    int contextEndLine = 0;
    std::string sourceContext;
};

/// Shared helpers for source-location parsing and report keys.
struct SaberSliceExportUtil
{
    static bool parseLocField(const std::string& locJson, const char* key,
                              std::string& outStr, int& outInt);
    static bool parseLocFileLine(const std::string& locJson, std::string& file, u32_t& line);
    static bool parseLocFileLine(const std::string& locJson, std::string& file, int& line);
    static SaberSourceLoc locFromICFG(const ICFGNode* icfg);
    static void fillLocFromICFG(const ICFGNode* icfg, std::string& file, int& line, int& col);
    static std::string makeICFGPairLocKey(const ICFGNode* lhs, const ICFGNode* rhs);
    static void collectPathConditions(const GenericBug::EventStack& eventStack,
                                      std::vector<SaberPathEdge>& out);
};

/// One extracted slice for a static SABER report.
struct SaberSlice
{
    std::string id;
    SaberSliceKind kind = SaberSliceKind::USE_AFTER_FREE;
    std::string staticVerdict = "REPORTED";

    std::string sourceFile;
    int sourceLine = 0;
    int sourceCol = 0;

    std::string useFile;
    int useLine = 0;
    int useCol = 0;

    /// UAF
    std::string reportKind = "alloc_source";
    std::string freeFile;
    int freeLine = 0;

    /// UNINIT
    std::string sourceKind = "unknown";
    std::string allocator = "unknown";
    bool zeroing = false;
    /// UNINIT: source-level identification recovered from debug info so that
    /// reports funneled through a shared inlined sink (e.g. the streaming logger)
    /// remain distinguishable. `variable` is the declared name (e.g. "retryBudget"),
    /// `sourceFunc` the declaring function, `callerFile/Line` the project-code call
    /// site that feeds the funnel when recoverable from the value-flow path.
    std::string variable;
    std::string sourceFunc;
    std::string callerFile;
    int callerLine = 0;
    /// UNINIT: the abstract type of the uninit stack object (e.g. "FalconLog",
    /// "std::string"). Acts as the "constructor / creation-site" key: many reports
    /// share one object type funneled to one sink, so grouping by object type
    /// collapses the logger funnel far more than per-function grouping.
    std::string objectType;
    std::string objectDescriptor;

    /// MEMORY_LEAK: NEVERFREE | PARTIALLEAK
    std::string leakKind;

    /// DOUBLE_FREE: second free site when known
    std::string free2File;
    int free2Line = 0;

    std::vector<SaberPathEdge> pathConditions;
    std::vector<SaberTraceNode> callTrace;
    std::vector<std::string> potentialFreeLocs;
    std::vector<std::string> potentialFreeContexts;
    std::vector<std::vector<SaberPathEdge>> safePathConditions;
    std::string bypassReturnLoc;

    std::string toJson(const std::string& indent) const;
    std::string stableIdentity() const;
};

/// Queued SABER report emitted at finalize; slice context is collected on flush.
struct SaberPendingReport
{
    GenericBug::BugType bugType = GenericBug::NEVERFREE;
    GenericBug::EventStack eventStack;

    /// Deferred terminal lines for leak/dfree sink sites (printed with the bug).
    std::vector<std::string> sinkLocs;
    std::vector<GenericBug::EventStack> sinkPathEvents;
    std::string sinkBypassReturnLoc;

    std::string uafReportKind;
    std::string uafObjectDescriptor;

    const SVFGNode* uninitSource = nullptr;
    std::string uninitSourceKind;
    std::string uninitAllocator;
    bool uninitZeroing = false;
};

class SaberSliceCollector
{
public:
    void setAlertOutDir(const std::string& path) { alertOutDir = path; }
    const std::string& alertOutDirRef() const { return alertOutDir; }

    bool collectUAFSlice(const GenericBug::EventStack& eventStack,
                         const std::string& reportKind, SaberSlice& out) const;
    bool collectUninitSlice(const SVFGNode* source, const GenericBug::EventStack& eventStack,
                            const std::string& sourceKind, const std::string& allocator,
                            bool zeroing, SaberSlice& out) const;
    bool collectLeakSlice(GenericBug::BugType bugType,
                          const GenericBug::EventStack& eventStack, SaberSlice& out) const;
    bool collectDoubleFreeSlice(const GenericBug::EventStack& eventStack, SaberSlice& out) const;
    bool collectSliceForPending(const SaberPendingReport& pending, SaberSlice& out) const;

    void addSlice(const SaberSlice& s) { slices.push_back(s); }
    bool empty() const { return slices.empty(); }
    size_t size() const { return slices.size(); }
    const std::vector<SaberSlice>& getSlices() const { return slices; }

    bool writeAlerts(const char* generatedBy) const;

private:
    std::string readCodeSnippet(const std::string& file, int line) const;
    void addSourceContext(std::vector<SaberTraceNode>& trace) const;

    std::string alertOutDir;
    std::vector<SaberSlice> slices;
};

} // namespace SVF

#endif /* SABER_SLICE_EXPORT_H_ */

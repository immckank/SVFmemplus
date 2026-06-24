//===- UninitLLMTriage.h -- LLM-assisted triage overlay for UninitChecker ---===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

#ifndef UNINIT_LLM_TRIAGE_H_
#define UNINIT_LLM_TRIAGE_H_

#include "Util/SVFBugReport.h"
#include "Graphs/SVFG.h"
#include <map>
#include <string>
#include <vector>

namespace SVF
{

class ICFGNode;
class ProgSlice;

/// One path-condition edge serialised for the LLM.
struct UninitPathEdge
{
    std::string location;
    std::string condition;
};

/// One extracted slice for a static UNINIT report.
struct UninitSlice
{
    std::string id;
    std::string sourceKind = "unknown";
    std::string staticVerdict = "REPORTED";

    std::string sourceFile;
    int sourceLine = 0;
    int sourceCol = 0;

    std::string useFile;
    int useLine = 0;
    int useCol = 0;

    std::string allocator = "unknown";
    bool zeroing = false;

    std::vector<UninitPathEdge> pathConditions;
    std::string codeSnippet;

    std::string toJson(const std::string& indent) const;
};

/// LLM endpoint / sidecar configuration.
struct UninitLLMTriageConfig
{
    std::string apiUrl;
    std::string apiKey;
    std::string model;
    double thresholdFp = 0.85;
    double thresholdConfirm = 0.70;
    std::string sliceOutPath = "uninit_slices.json";
    std::string verdictPath = "uninit_verdicts.json";
    std::string sidecarPath;
    std::string pythonExe = "python3";
    bool suppressFp = true;
    bool upgradeConfirm = true;

    bool loadFromFile(const std::string& path);
    void loadFromEnv();
    bool hasApi() const;
};

/// Verdict returned by the sidecar for one slice id.
struct UninitLLMVerdict
{
    std::string id;
    std::string verdict = "UNCERTAIN"; ///< TRUE_UNINIT | LIKELY_FP | UNCERTAIN
    double confidence = 0.0;
    std::string rationale;
};

class UninitLLMTriage
{
public:
    void setConfig(const UninitLLMTriageConfig& c) { cfg = c; }
    const UninitLLMTriageConfig& config() const { return cfg; }

    bool collectSlice(const SVFGNode* source, const GenericBug::EventStack& eventStack,
                      const std::string& sourceKind, const std::string& allocator,
                      bool zeroing, UninitSlice& out) const;

    void addSlice(const UninitSlice& s) { slices.push_back(s); }
    bool empty() const { return slices.empty(); }
    size_t size() const { return slices.size(); }
    const std::vector<UninitSlice>& getSlices() const { return slices; }

    bool writeSlices() const;
    bool runSidecarAndLoad(std::map<std::string, UninitLLMVerdict>& out) const;

    static bool isWhitelistedTruePositive(const std::string& useFile, int useLine);

private:
    std::string readCodeSnippet(const std::string& file, int line) const;
    static bool parseLocField(const std::string& locJson, const char* key, std::string& outStr, int& outInt);

    UninitLLMTriageConfig cfg;
    std::vector<UninitSlice> slices;
};

} // namespace SVF

#endif /* UNINIT_LLM_TRIAGE_H_ */

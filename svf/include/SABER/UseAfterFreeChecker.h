// svf/include/UseAfterFreeChecker.h
#ifndef USEAFTERFREECHECKER_H_
#define USEAFTERFREECHECKER_H_

#include "SABER/LeakChecker.h"
#include <cstdint>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

namespace SVF
{

class UseAfterFreeChecker : public LeakChecker
{

public:
    UseAfterFreeChecker(): LeakChecker(), freeAsSourceMode_(false)
    {
    }

    virtual ~UseAfterFreeChecker()
    {
    }

    virtual bool runOnModule(SVFIR* pag) override
    {
        analyze();
        return false;
    }

    virtual void analyze() override;
    void reportBug(ProgSlice* slice) override;

    virtual void initSnks() override;

    inline const SVFGNodeSet& getFreeNodes() const
    {
        return freeNodes;
    }
    inline SVFGNodeSetIter freeNodesBegin() const
    {
        return freeNodes.begin();
    }
    inline SVFGNodeSetIter freeNodesEnd() const
    {
        return freeNodes.end();
    }
    inline void addToFreeNodes(const SVFGNode* node)
    {
        freeNodes.insert(node);
    }
    inline const SVFGNodeSet& getUseNodes() const
    {
        return useNodes;
    }
    inline SVFGNodeSetIter useNodesBegin() const
    {
        return useNodes.begin();
    }
    inline SVFGNodeSetIter useNodesEnd() const
    {
        return useNodes.end();
    }
    inline void addToUseNodes(const SVFGNode* node)
    {
        useNodes.insert(node);
    }

    bool isSatisfiableForFreeAndUsePairs(ProgSlice* slice, GenericBug::EventStack& eventStack);

protected:
    bool includeUncalledAllocSources() const override
    {
        return true;
    }

    void setCurSlice(const SVFGNode* src) override;
    void FWProcessCurNode(const DPIm& item) override;

    bool enableReachGlobalPrune() const override
    {
        return false;
    }
    virtual bool enableSliceLocalUAFPairing() const
    {
        return true;
    }

    const char* sliceExportGeneratedBy() const override;
    void onPendingReportsFlushed(u32_t pendingCount, u32_t emittedCount) override;

private:
    struct PairHash
    {
        size_t operator()(const std::pair<NodeID, NodeID>& p) const
        {
            return (static_cast<size_t>(p.first) << 32) ^ static_cast<size_t>(p.second);
        }
    };

    static const SVFVar* getFreedPointerVar(const SVFGNode* freeNode);
    static const SVFVar* getUsePointerVar(const SVFGNode* useNode);
    bool maySameFreedObject(const SVFGNode* freeNode, const SVFGNode* useNode) const;
    static ProgSlice::Condition computeControlOrderGuard(ProgSlice* slice,
            const SVFGNode* freeNode, const SVFGNode* useNode);
    bool isFeasibleFreeUsePair(ProgSlice* slice, const SVFGNode* freeNode,
                               const SVFGNode* useNode, ProgSlice::Condition& outGuard) const;
    bool isOrderedValueFlowUAFPair(const SVFGNode* freeNode, const SVFGNode* useNode) const;

    void analyzeFreeAnchoredUAFPairs();
    void runSliceFromSource(const SVFGNode* source, bool freeAsSource);
    void collectObjectCandidateUses(const SVFGNodeSet& candidateUseSet,
                                    const SVFGNode* freeNode,
                                    std::vector<const SVFGNode*>& outUses) const;
    void collectObjectCandidateUses(const std::vector<const SVFGNode*>& candidateUseSet,
                                    const SVFGNode* freeNode,
                                    std::vector<const SVFGNode*>& outUses) const;
    bool isDuplicateUAFPair(const SVFGNode* freeNode, const SVFGNode* useNode) const;
    void markUAFPairReported(const SVFGNode* freeNode, const SVFGNode* useNode);
    static const CallICFGNode* getFreeCallICFGNode(const SVFGNode* freeNode);
    std::vector<const SVFGNode*> getCallerActualParmAnchors(const SVFGNode* freeNode) const;
    void tryReportCrossFunctionUAF(const SVFGNode* freeNode, const SVFGNode* useNode);
    bool isUseAfterFreeCallInCaller(const SVFGNode* freeNode, const SVFGNode* useNode) const;
    bool isDirectCallerUseOfFreeFunction(const SVFGNode* freeNode, const SVFGNode* useNode) const;
    bool isFieldLoadUseOfFreedBase(const SVFGNode* freeNode,
                                   const SVFGNode* useNode) const;
    bool isNestedFieldFreePointer(const SVFGNode* freeNode) const;
    bool shouldSkipCrossFileAllocNoise(const SVFGNode* freeNode,
                                       const SVFGNode* useNode) const;

    void queueUAFReport(GenericBug::EventStack eventStack, const std::string& reportKind);

    SVFGNodeSet freeNodes;
    SVFGNodeSet useNodes;
    bool freeAsSourceMode_;
    std::unordered_set<std::pair<NodeID, NodeID>, PairHash> reportedUAFPairs_;
};

} // End namespace SVF

#endif /* USEAFTERFREECHECKER_H_ */

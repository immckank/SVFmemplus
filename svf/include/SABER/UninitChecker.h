// svf/include/UninitChecker.h
#ifndef UNINITCHECKER_H_
#define UNINITCHECKER_H_

#include "SABER/LeakChecker.h"
#include "SABER/UninitLLMTriage.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SVF
{

class UninitChecker : public LeakChecker
{

public:
    UninitChecker();
    virtual ~UninitChecker();

    virtual bool runOnModule(SVFIR* pag) override;
    virtual void analyze() override;
    void reportBug(ProgSlice* slice) override;
    void finalize() override;

    static void setLLMTriageConfig(const UninitLLMTriageConfig& cfg);

    virtual void initSrcs() override;
    virtual void initSnks() override;

    inline const SVFGNodeSet& getStoreNodes() const
    {
        return storeNodes;
    }
    inline SVFGNodeSetIter storeNodesBegin() const
    {
        return storeNodes.begin();
    }
    inline SVFGNodeSetIter storeNodesEnd() const
    {
        return storeNodes.end();
    }
    inline void addToStoreNodes(const SVFGNode* node)
    {
        storeNodes.insert(node);
    }
    inline const SVFGNodeSet& getLoadNodes() const
    {
        return loadNodes;
    }
    inline SVFGNodeSetIter loadNodesBegin() const
    {
        return loadNodes.begin();
    }
    inline SVFGNodeSetIter loadNodesEnd() const
    {
        return loadNodes.end();
    }
    inline void addToLoadNodes(const SVFGNode* node)
    {
        loadNodes.insert(node);
    }

    bool hasFeasibleUninitPath(ProgSlice* rawSlice, ProgSlice* guardSlice,
                               const SVFGNode* load,
                               GenericBug::EventStack& eventStack);

protected:
    bool needDefaultAllPathSolve() const override
    {
        return false;
    }

    bool enableReachGlobalPrune() const override
    {
        return false;
    }

private:
    struct RegionKey
    {
        enum Precision
        {
            WholeObject,
            Field,
            CollapsedObject,
            Unknown
        };

        NodeID base = 0;
        APOffset field = 0;
        Precision precision = WholeObject;

        RegionKey() = default;
        RegionKey(NodeID b, APOffset f, Precision p) : base(b), field(f), precision(p) {}

        bool operator==(const RegionKey& rhs) const
        {
            return base == rhs.base && field == rhs.field && precision == rhs.precision;
        }
    };

    struct RegionKeyHash
    {
        std::size_t operator()(const RegionKey& region) const
        {
            return std::hash<NodeID>()(region.base) ^
                   (std::hash<APOffset>()(region.field) << 1) ^
                   (std::hash<unsigned>()(static_cast<unsigned>(region.precision)) << 2);
        }
    };

    struct PendingUninitReport
    {
        GenericBug::EventStack eventStack;
        const SVFGNode* source = nullptr;
        std::string sourceKind;
        std::string allocator;
        bool zeroing = false;
        std::string reportKey;
        UninitSlice slice;
    };

    typedef std::unordered_set<RegionKey, RegionKeyHash> RegionSet;
    typedef std::unordered_map<const SVFGNode*, RegionSet> NodeToRegionStateMap;

    enum StoreBlockReason
    {
        StoreBlocks,
        NotStoreNode,
        NotStrongStore,
        EmptyRegionSet,
        RegionDoesNotIntersect,
        RHSMayCarryUninit
    };

    enum StrongUpdateFailureReason
    {
        StrongOK,
        StrongNotStore,
        StrongNoPTA,
        StrongMultiPts,
        StrongMultiRegion,
        StrongCollapsedRegion,
        StrongUnknownRegion,
        StrongFieldInsensitive,
        StrongVarArray
    };

    struct StoreBlockDebugStats
    {
        u32_t checks = 0;
        u32_t storeNodes = 0;
        u32_t strongStores = 0;
        u32_t regionIntersects = 0;
        u32_t blockers = 0;
        u32_t notStore = 0;
        u32_t notStrong = 0;
        u32_t strongMultiPts = 0;
        u32_t strongMultiRegion = 0;
        u32_t strongCollapsedRegion = 0;
        u32_t strongUnknownRegion = 0;
        u32_t strongFieldInsensitive = 0;
        u32_t strongVarArray = 0;
        u32_t emptyRegion = 0;
        u32_t regionMiss = 0;
        u32_t rhsMayCarry = 0;
        u32_t reachedSource = 0;
        u32_t visitedBackward = 0;
    };

    void flushPendingReports();
    std::string makeReportKey(const ICFGNode* sourceICFG, const ICFGNode* useICFG) const;
    bool shouldSkipHeaderSourceReport(const ICFGNode* sourceICFG, const ICFGNode* useICFG) const;
    std::string classifySourceKind(const SVFGNode* source) const;
    std::string classifyAllocator(const SVFGNode* source) const;
    bool isPlainKmallocAllocatorName(const std::string& name) const;
    bool isMemsetLikeInitializingStore(const SVFGNode* store) const;

    void collectCandidateLoads(const SVFGNodeSet& qualifierStateIgnorePtrStore,
                               const SVFGNodeSet& qualifierStateAllStore,
                               SVFGNodeSet& candidateLoads) const;
    void collectRegionCandidateLoads(ProgSlice* slice, SVFGNodeSet& candidateLoads) const;
    std::unique_ptr<ProgSlice> buildStoreBypassGuardSlice(ProgSlice* rawSlice,
                                                          const SVFGNode* load) const;
    SVFGNodeSet storeNodes;
    SVFGNodeSet loadNodes;
    SVFGNodeSet ptrStoreNodes;
    SVFGNodeSet ptrLoadNodes;
    SVFGNodeSet criticalSinkNodes;
    std::unordered_map<const SVFGNode*, RegionSet> sourceInitialRegions;
    mutable std::unordered_map<const SVFGNode*, RegionSet> loadReadRegionCache;
    mutable std::unordered_map<const SVFGNode*, RegionSet> storeWriteRegionCache;
    mutable std::unordered_map<const SVFGNode*, bool> ignorePtrStoreForLoadCache;
    mutable u32_t uninitDebugLinesPrinted = 0;
    u32_t smallInitSkippedSources = 0;
    u32_t dedupSkippedReports = 0;
    u32_t headerSkippedReports = 0;
    u32_t llmSuppressedReports = 0;
    u32_t emittedUninitReports = 0;
    std::unordered_map<u64_t, SVFGNodeSet> summaryBoundaryToLoads;
    std::unordered_map<u64_t, SVFGNodeSet> summaryBoundaryToBoundaries;
    std::unordered_set<std::string> reportedKeys;
    std::vector<PendingUninitReport> pendingReports;
    UninitLLMTriage llmTriage;
    static UninitLLMTriageConfig s_llmCfg;

    bool isPtrStoreNode(const SVFGNode* node) const;
    bool isPtrLoadNode(const SVFGNode* node) const;
    bool isCriticalUninitSink(const SVFGNode* load) const;
    bool flowsToCriticalUseWithinBudget(const SVFGNode* load, u32_t maxSteps) const;
    bool isMemsetLikeCall(const ICFGNode* node) const;
    bool isMemcpyLikeCall(const ICFGNode* node) const;
    bool isCopyFromUserLikeCall(const ICFGNode* node) const;
    bool isAPIInitStoreForLoad(const SVFGNode* load, const SVFGNode* store, ProgSlice* slice) const;
    bool shouldConsiderStoreForSummaryMode(const SVFGNode* node, bool ignorePtrStore) const;
    bool isSummaryBoundaryNode(const SVFGNode* node) const;
    u64_t getSummaryKey(const SVFGNode* node, bool ignorePtrStore) const;
    void getOrBuildSummaryForBoundary(const SVFGNode* boundary, bool ignorePtrStore,
                                      const SVFGNodeSet*& reachableLoads,
                                      const SVFGNodeSet*& nextBoundaries);
    bool shouldIgnorePtrStoreForLoad(const SVFGNode* load) const;
    bool shouldConsiderStoreForMode(const SVFGNode* store, ProgSlice* slice, bool ignorePtrStore) const;
    bool shouldConsiderStoreForLoad(const SVFGNode* load, const SVFGNode* store, ProgSlice* slice) const;
    StoreBlockReason classifyStoreBlocker(const SVFGNode* load, const SVFGNode* store, ProgSlice* slice) const;
    StrongUpdateFailureReason classifyStrongUpdateFailure(const SVFGNode* store) const;
    const char* strongUpdateFailureName(StrongUpdateFailureReason reason) const;
    const char* storeBlockReasonName(StoreBlockReason reason) const;
    void updateStoreBlockDebugStats(StoreBlockDebugStats& stats, StoreBlockReason reason,
                                    const SVFGNode* store) const;
    void debugCandidateBlockers(ProgSlice* slice, const SVFGNode* load) const;
    bool sameBasicBlockReachableBefore(const ICFGNode* beforeNode, const ICFGNode* afterNode) const;
    bool sameFunctionDominates(const ICFGNode* domNode, const ICFGNode* useNode) const;
    bool hasDominatingInitBlocker(ProgSlice* slice, const SVFGNode* load) const;
    bool inUninitCandidateSlice(ProgSlice* slice, const SVFGNode* node) const;
    RegionKey makeWholeRegion(NodeID obj) const;
    RegionKey makeFieldRegion(NodeID obj, APOffset field) const;
    RegionKey makeCollapsedRegion(NodeID obj) const;
    RegionKey makeUnknownRegion(NodeID obj) const;
    bool regionsMayIntersect(const RegionKey& lhs, const RegionKey& rhs) const;
    bool regionSetsMayIntersect(const RegionSet& lhs, const RegionSet& rhs) const;
    bool regionCoveredByWrite(const RegionKey& region, const RegionKey& writeRegion) const;
    bool regionSetsCover(const RegionSet& regions, const RegionSet& writeRegions) const;
    bool isSameAddressStoreLoad(const SVFGNode* store, const SVFGNode* load) const;
    bool isLoadSpecificStoreKill(const SVFGNode* load, const SVFGNode* store, ProgSlice* slice) const;
    bool eraseInitializedRegions(RegionSet& state, const RegionSet& writeRegions) const;
    bool getStoreStmtWriteRegions(const StoreStmt* store, RegionSet& regions) const;
    bool storeStmtRHSMayBeUninitSource(const StoreStmt* store, const SVFGNode* source) const;
    bool isMustExecuteDirectInitStore(const StoreStmt* store, const BaseObjVar* obj) const;
    bool refineInitialRegionsWithDirectStores(const BaseObjVar* obj, const SVFGNode* source,
                                              RegionSet& regions) const;
    bool mergeRegionState(NodeToRegionStateMap& states, const SVFGNode* node, const RegionSet& incoming) const;
    bool storeRHSMayCarryUninitInState(const SVFGNode* store, ProgSlice* slice,
                                       const NodeToRegionStateMap& states) const;
    void computeRegionUninitState(ProgSlice* slice, NodeToRegionStateMap& states) const;
    bool loadMayReadUninitRegion(const SVFGNode* load, const NodeToRegionStateMap& states) const;
    bool addPointeeRegions(NodeID ptr, RegionSet& regions) const;
    bool addObjectRegion(NodeID obj, RegionSet& regions) const;
    bool getInitialRegionsForSource(const SVFGNode* source, RegionSet& regions) const;
    const RegionSet& getLoadReadRegions(const SVFGNode* load) const;
    const RegionSet& getStoreWriteRegions(const SVFGNode* store) const;
    bool storeMayKillLoadRegion(const SVFGNode* store, const SVFGNode* load) const;
    bool isStoreStrongRegionKill(const SVFGNode* store) const;
    bool storeRHSMayCarryUninit(const SVFGNode* store, ProgSlice* slice) const;
    bool isZeroingAllocatorName(const std::string& name) const;
    bool isZeroingHeapObject(const HeapObjVar* heapObj) const;
    void computeQualifierInferenceState(ProgSlice* slice, bool ignorePtrStore, SVFGNodeSet& mayUninitReachable);
    bool isDefinitelyInitInComputedState(const SVFGNodeSet& mayUninitReachable, const SVFGNode* load) const;
    bool isFormalParameterPointerLoad(const SVFGNode* load) const;
    bool isPtrLoadAddressComputationOnly(const SVFGNode* load) const;
    bool isDirectParameterSpillLoad(const SVFGNode* load) const;
    bool isParameterSpillStackObject(StackObjVar* stackObj, SVFIR* pag) const;
    bool isSmallDirectlyInitializedStackObject(StackObjVar* stackObj, SVFIR* pag) const;
    bool isCompositeMemoryObject(const BaseObjVar* obj) const;
    bool isSmallScalarStackObject(const BaseObjVar* obj) const;
    bool pagNodeMayReachCallPE(const SVFVar* node, SVFIR* pag) const;
    bool isAddressEscapingScalarStackObject(StackObjVar* stackObj, SVFIR* pag) const;
    bool isTrackableScalarStackObject(StackObjVar* stackObj, SVFIR* pag) const;
    bool isScalarStackValueLoad(const SVFGNode* load) const;
    bool pointeeIncludesCompositeObject(NodeID ptr) const;
    bool isReturnAnchoredICFGNode(const ICFGNode* node) const;
};

template<class Data>
class VisitedFIFOWorkList
{
    typedef std::deque<Data> DataDeque;
    typedef std::unordered_set<Data> DataSet;

public:
    VisitedFIFOWorkList() {}
    ~VisitedFIFOWorkList() {}

    inline bool empty() const
    {
        return data_list.empty();
    }

    inline u32_t size() const
    {
        return data_list.size();
    }

    inline bool isVisited(const Data &data) const
    {
        return visited.find(data) != visited.end();
    }

    inline bool push(const Data &data)
    {
        if (visited.insert(data).second) {
            data_list.push_back(data);
            return true;
        }
        return false;
    }

    inline Data pop()
    {
        assert(!empty() && "work list is empty");
        Data data = data_list.front();
        data_list.pop_front();
        return data;
    }

    inline void clear()
    {
        data_list.clear();
        visited.clear();
    }

private:
    DataDeque data_list;
    DataSet visited;
};

} // End namespace SVF

#endif /* UNINITCHECKER_H_ */

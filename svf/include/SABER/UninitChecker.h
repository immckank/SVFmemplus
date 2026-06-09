// svf/include/UninitChecker.h
#ifndef UNINITCHECKER_H_
#define UNINITCHECKER_H_

#include "SABER/LeakChecker.h"
#include <memory>
#include <unordered_map>

namespace SVF
{

/*!
 * Double free checker to check deallocations of memory
 */

class UninitChecker : public LeakChecker
{

public:
    UninitChecker();
    virtual ~UninitChecker();

    /// We start from here
    virtual bool runOnModule(SVFIR* pag) override;
    virtual void analyze() override;

    /// Report file/close bugs
    void reportBug(ProgSlice* slice) override;


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
    void collectCandidateLoads(const SVFGNodeSet& qualifierStateIgnorePtrStore,
                               const SVFGNodeSet& qualifierStateAllStore,
                               SVFGNodeSet& candidateLoads) const;
    std::unique_ptr<ProgSlice> buildStoreBypassGuardSlice(ProgSlice* rawSlice,
                                                          const SVFGNode* load) const;
    SVFGNodeSet storeNodes;
    SVFGNodeSet loadNodes;
    SVFGNodeSet ptrStoreNodes;
    SVFGNodeSet ptrLoadNodes;
    mutable std::unordered_map<const SVFGNode*, bool> ignorePtrStoreForLoadCache;
    std::unordered_map<u64_t, SVFGNodeSet> summaryBoundaryToLoads;
    std::unordered_map<u64_t, SVFGNodeSet> summaryBoundaryToBoundaries;
    bool isPtrStoreNode(const SVFGNode* node) const;
    bool isPtrLoadNode(const SVFGNode* node) const;
    bool shouldConsiderStoreForSummaryMode(const SVFGNode* node, bool ignorePtrStore) const;
    bool isSummaryBoundaryNode(const SVFGNode* node) const;
    u64_t getSummaryKey(const SVFGNode* node, bool ignorePtrStore) const;
    void getOrBuildSummaryForBoundary(const SVFGNode* boundary, bool ignorePtrStore,
                                      const SVFGNodeSet*& reachableLoads,
                                      const SVFGNodeSet*& nextBoundaries);
    bool shouldIgnorePtrStoreForLoad(const SVFGNode* load) const;
    bool shouldConsiderStoreForMode(const SVFGNode* store, ProgSlice* slice, bool ignorePtrStore) const;
    bool shouldConsiderStoreForLoad(const SVFGNode* load, const SVFGNode* store, ProgSlice* slice) const;
    bool inUninitCandidateSlice(ProgSlice* slice, const SVFGNode* node) const;
    void computeQualifierInferenceState(ProgSlice* slice, bool ignorePtrStore, SVFGNodeSet& mayUninitReachable);
    bool isDefinitelyInitInComputedState(const SVFGNodeSet& mayUninitReachable, const SVFGNode* load) const;
    bool isFormalParameterPointerLoad(const SVFGNode* load) const;
    bool isPtrLoadAddressComputationOnly(const SVFGNode* load) const;
    bool isDirectParameterSpillLoad(const SVFGNode* load) const;
    bool isParameterSpillStackObject(StackObjVar* stackObj, SVFIR* pag) const;

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

    /**
     * 是否已经访问过该节点（无论是否仍在队列中）
     */
    inline bool isVisited(const Data &data) const
    {
        return visited.find(data) != visited.end();
    }

    /**
     * 向队列中推入一个节点（若从未访问过）
     */
    inline bool push(const Data &data)
    {
        // 只要访问过，就不再重复入队
        if (visited.insert(data).second) {
            data_list.push_back(data);
            return true;
        }
        return false;
    }

    /**
     * 取出队首元素（不会影响 visited 状态）
     */
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
    DataDeque data_list;   ///< 按FIFO顺序的队列
    DataSet visited;       ///< 所有已访问过的节点（包括已出队）
};


} // End namespace SVF

#endif /* UNINITCHECKER_H_ */

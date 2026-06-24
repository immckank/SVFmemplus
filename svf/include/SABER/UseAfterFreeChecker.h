// svf/include/UseAfterFreeChecker.h
#ifndef USEAFTERFREECHECKER_H_
#define USEAFTERFREECHECKER_H_

#include "SABER/LeakChecker.h"

namespace SVF
{

/*!
 * Double free checker to check deallocations of memory
 */

class UseAfterFreeChecker : public LeakChecker
{

public:
    /// Constructor
    UseAfterFreeChecker(): LeakChecker()
    {
    }

    /// Destructor
    virtual ~UseAfterFreeChecker()
    {
    }

    /// We start from here
    virtual bool runOnModule(SVFIR* pag) override
    {
        /// start analysis
        analyze();
        return false;
    }

    virtual void analyze() override;

    /// Report file/close bugs
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

private:
    static const SVFVar* getFreedPointerVar(const SVFGNode* freeNode);
    static const SVFVar* getUsePointerVar(const SVFGNode* useNode);
    static bool maySameFreedObject(const SVFG* svfg, const SVFGNode* freeNode,
                                   const SVFGNode* useNode);
    static ProgSlice::Condition computeControlOrderGuard(ProgSlice* slice,
            const SVFGNode* freeNode, const SVFGNode* useNode);
    bool isFeasibleFreeUsePair(ProgSlice* slice, const SVFGNode* freeNode,
                               const SVFGNode* useNode, ProgSlice::Condition& outGuard) const;

    SVFGNodeSet freeNodes;
    SVFGNodeSet useNodes;

};

} // End namespace SVF

#endif /* USEAFTERFREECHECKER_H_ */

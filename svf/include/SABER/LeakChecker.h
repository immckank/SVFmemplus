//===- LeakChecker.h -- Detecting memory leaks--------------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

/*
 * LeakChecker.h
 *
 *  Created on: Apr 1, 2014
 *      Author: rockysui
 */

#ifndef LEAKCHECKER_H_
#define LEAKCHECKER_H_

#include "SABER/SrcSnkDDA.h"
#include "SABER/SaberCheckerAPI.h"
#include "SABER/SaberSliceExport.h"
#include "Util/Options.h"
#include <unordered_set>
#include <vector>

namespace SVF
{

/*!
 * Static Memory Leak Detector
 */
class LeakChecker : public SrcSnkDDA
{

public:
    typedef Map<const SVFGNode*,const CallICFGNode*> SVFGNodeToCSIDMap;
    typedef FIFOWorkList<const CallICFGNode*> CSWorkList;
    typedef ProgSlice::VFWorkList WorkList;
    typedef NodeBS SVFGNodeBS;
    enum LEAK_TYPE
    {
        NEVER_FREE_LEAK,
        CONTEXT_LEAK,
        PATH_LEAK,
        GLOBAL_LEAK
    };

    /// Constructor
    LeakChecker()
    {
        Options::SABERFULLSVFG.setValue(true);
    }
    /// Destructor
    virtual ~LeakChecker()
    {
    }

    /// We start from here
    virtual bool runOnModule(SVFIR* pag)
    {
        /// start analysis
        analyze();
        return false;
    }

    /// Initialize sources and sinks
    //@{
    /// Initialize sources and sinks
    virtual void initSrcs() override;
    virtual void initSnks() override;
    /// Whether the function is a heap allocator/reallocator (allocate memory)
    virtual inline bool isSourceLikeFun(const FunObjVar* fun) override
    {
        return SaberCheckerAPI::getCheckerAPI()->isMemAlloc(fun);
    }
    /// Whether the function is a heap deallocator (free/release memory)
    virtual inline bool isSinkLikeFun(const FunObjVar* fun) override
    {
        return SaberCheckerAPI::getCheckerAPI()->isMemDealloc(fun);
    }
    //@}

    static void setSliceExportPath(const std::string& path);
    virtual void finalize() override;

protected:
    /// Whether initSrcs should include allocation sites in functions without callers.
    virtual bool includeUncalledAllocSources() const
    {
        return Options::RunUncallFuncs();
    }

    bool enableReachGlobalPrune() const override
    {
        return true;
    }

    /// Report leaks
    //@{
    virtual void reportBug(ProgSlice* slice) override;
    //@}

    bool hasSinkBypassReturn(const ProgSlice* slice, const ICFGNode*& bypassRet) const;
    std::string getSinkNodeLoc(const SVFGNode* snk) const;
    const ICFGNode* getSinkICFGNode(const SVFGNode* snk) const;
    bool isOwnershipTransferBarrier(const ICFGNode* node) const;

    /// Validate test cases for regression test purpose
    void testsValidation(const ProgSlice* slice);
    void validateSuccessTests(const SVFGNode* source, const FunObjVar* fun);
    void validateExpectedFailureTests(const SVFGNode* source, const FunObjVar* fun);

    bool sliceExportEnabled() const;
    void prepareSliceCollector();
    virtual const char* sliceExportGeneratedBy() const;
    void clearPendingReports();
    bool queuePendingReport(SaberPendingReport&& report, const std::string& dedupKey);
    void flushPendingReports();
    virtual void onPendingReportsFlushed(u32_t pendingCount, u32_t emittedCount) {}

    /// Record a source to its callsite
    //@{
    inline void addSrcToCSID(const SVFGNode* src, const CallICFGNode* cs)
    {
        srcToCSIDMap[src] = cs;
    }
    inline const CallICFGNode* getSrcCSID(const SVFGNode* src) const
    {
        SVFGNodeToCSIDMap::const_iterator it =srcToCSIDMap.find(src);
        assert(it!=srcToCSIDMap.end() && "source node not at a callsite??");
        return it->second;
    }
    //@}
    SaberSliceCollector sliceCollector_;
    static std::string s_sliceOutPath_;
    static bool s_sliceExportConfigured_;
    std::vector<SaberPendingReport> pendingReports_;

private:
    void collectSliceForPending(const SaberPendingReport& pending);

    SVFGNodeToCSIDMap srcToCSIDMap;
    std::unordered_set<std::string> pendingReportKeys_;
};

} // End namespace SVF

#endif /* LEAKCHECKER_H_ */

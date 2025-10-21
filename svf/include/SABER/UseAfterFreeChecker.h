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

    /// Report file/close bugs
    void reportBug(ProgSlice* slice) override;


    virtual void initSrcs() override;
    virtual void initSnks() override;
};

} // End namespace SVF

#endif /* USEAFTERFREECHECKER_H_ */

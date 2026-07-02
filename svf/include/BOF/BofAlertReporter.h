//===- BofAlertReporter.h -- BOF unified alert serialization --------------===//

#ifndef BOF_ALERT_REPORTER_H_
#define BOF_ALERT_REPORTER_H_

#include "BOF/LLMTriage.h"

namespace SVF
{

class BofAlertReporter
{
public:
    static bool write(const LLMTriageConfig& config,
                      const std::vector<BofSlice>& slices);
};

} // namespace SVF

#endif

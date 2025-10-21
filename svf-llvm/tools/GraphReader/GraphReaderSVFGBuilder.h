#ifndef GRAPHREADERSVFGBUILDER_H_
#define GRAPHREADERSVFGBUILDER_H_

#include "SABER/SaberSVFGBuilder.h"

namespace SVF
{

/*!
 * \brief SVFG Builder for GraphReader.
 *
 * This builder inherits from SaberSVFGBuilder but overrides buildSVFG
 * to perform only the standard SVFG construction without Saber-specific
 * modifications. This ensures that GraphReader works with a clean,
 * unmodified SVFG, avoiding dependencies on components like SaberCondAllocator.
 */
class GraphReaderSVFGBuilder : public SaberSVFGBuilder
{
protected:
    /// Override to build a standard SVFG without Saber-specific modifications.
    void buildSVFG() override;
};

} // namespace SVF

#endif /* GRAPHREADERSVFGBUILDER_H_ */
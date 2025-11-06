#ifndef GRAPHREADERSVFGBUILDER_H_
#define GRAPHREADERSVFGBUILDER_H_

#include "MSSA/SVFGBuilder.h"

namespace SVF
{

/*!
 * \brief SVFG Builder specifically designed for GraphReader.
 *
 * This builder extends SVFGBuilder with GraphReader-specific optimizations
 * and features, while maintaining compatibility with the standard SVFG
 * construction process.
 *
 * Design rationale:
 * - Inherits from SVFGBuilder (not SaberSVFGBuilder) for clean separation
 * - Provides extension points for GraphReader-specific analysis
 * - Can selectively adopt Saber features when needed (via composition)
 * - Avoids dependencies on Saber components (SaberCondAllocator, etc.)
 *
 * Future extensions can include:
 * - Custom SVFG optimizations for query performance
 * - GraphReader-specific node/edge filtering
 * - Incremental SVFG updates for interactive queries
 */
class GraphReaderSVFGBuilder : public SVFGBuilder
{
public:
    /// Constructor
    explicit GraphReaderSVFGBuilder(bool _SVFGWithIndCall = true, bool _SVFGWithPostOpts = false)
        : SVFGBuilder(_SVFGWithIndCall, _SVFGWithPostOpts),
          enableSaberOptimizations(false)
    {
    }

    /// Destructor
    virtual ~GraphReaderSVFGBuilder() = default;

    /*!
     * \brief Enable or disable Saber-like optimizations.
     * 
     * When enabled, applies selective optimizations from SaberSVFGBuilder
     * that are safe and beneficial for GraphReader queries.
     * 
     * \param enable true to enable optimizations, false otherwise
     */
    void setEnableSaberOptimizations(bool enable)
    {
        enableSaberOptimizations = enable;
    }

protected:
    /// Override to add GraphReader-specific SVFG construction logic
    virtual void buildSVFG() override;

    /*!
     * \brief Apply optional Saber-inspired optimizations.
     * 
     * This method selectively applies beneficial optimizations from
     * SaberSVFGBuilder without requiring SaberCondAllocator or other
     * Saber-specific dependencies.
     * 
     * Currently includes:
     * - Global variable collection (optional)
     * - Dereference edge removal (optional)
     */
    virtual void applySaberOptimizations(BVDataPTAImpl* pta);

    /// Remove dereference direct SVFG edges (adapted from SaberSVFGBuilder)
    void rmDerefDirSVFGEdges(BVDataPTAImpl* pta);

    /// Collect global variables (adapted from SaberSVFGBuilder)
    void collectGlobals(BVDataPTAImpl* pta);

    /// Add ActualParmVFGNode for deallocation/file-close functions (adapted from SaberSVFGBuilder)
    /// This is CRITICAL for value-flow analysis of free/fclose arguments
    void AddExtActualParmSVFGNodes(CallGraph* callgraph);

    /// Helper to add actual parameter node
    inline void addActualParmVFGNode(const PAGNode* pagNode, const CallICFGNode* cs)
    {
        svfg->addActualParmVFGNode(pagNode, cs);
    }

private:
    /// Flag to control whether to apply Saber-like optimizations
    bool enableSaberOptimizations;

    /// Store global variable IDs (only used when enableSaberOptimizations is true)
    NodeBS globs;
};

} // namespace SVF

#endif /* GRAPHREADERSVFGBUILDER_H_ */


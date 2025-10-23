#include "GraphReaderSVFGBuilder.h"
#include "Graphs/SVFG.h"

namespace SVF
{

void GraphReaderSVFGBuilder::buildSVFG()
{
    // Call the base SVFGBuilder::buildSVFG() to construct the graph without any Saber modifications.
    // This works because SVFGBuilder is a friend of SVFG and can call its protected buildSVFG method.
    SVFGBuilder::buildSVFG();
}

} // namespace SVF
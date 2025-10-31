#ifndef FUNCTION_QUERY_H
#define FUNCTION_QUERY_H

#include "Graphs/ICFG.h"
#include "SVFIR/SVFIR.h"
#include <string>

namespace SVF {

/*!
 * \class FunctionQuery
 * \brief A class to encapsulate various queries related to functions in the control-flow graph.
 *
 * This class provides methods to find call sites, function bodies by location or name,
 * and information about callees at a specific call site. It requires ICFG and PAG
 * for its operations.
 */
class FunctionQuery {
private:
    ICFG* icfg;
    SVFIR* pag;

public:
    FunctionQuery(ICFG* i, SVFIR* p);

    //! Find all call sites of a given function by its name.
    void findCallSites(const std::string& functionName);

    //! Find and print information about callee functions at a specific source location.
    void findCalleeBodyByLocation(const std::string& location);

    //! Find and print information about the function containing a specific source location.
    void findFunctionBodyByLocation(const std::string& location);

    //! Find and print information about a function by its name.
    void findFunctionBodyByName(const std::string& functionName);

    //! Find all functions that can be called by a given function (transitively).
    void findAllCalleesByName(const std::string& functionName);

    //! Find all return locations reachable from a given location within a function.
    void findRetLocations(const std::string& functionName, const std::string& location);
    
};

} // namespace SVF

#endif // FUNCTION_QUERY_H
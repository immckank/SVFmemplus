//===- SourceEvidence.h -- shared source-file evidence helpers ------------===//

#ifndef SOURCE_EVIDENCE_H_
#define SOURCE_EVIDENCE_H_

#include <string>

namespace SVF
{

struct SourceLocation
{
    std::string file;
    int line = 0;
    int column = 0;
};

SourceLocation parseSourceLocation(const std::string& encoded);

std::string readSourceSnippet(const std::string& file, int line,
                              int linesBefore, int linesAfter);

} // namespace SVF

#endif

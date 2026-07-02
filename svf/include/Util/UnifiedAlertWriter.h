//===- UnifiedAlertWriter.h -- shared one-file-per-alert persistence -------===//

#ifndef UNIFIED_ALERT_WRITER_H_
#define UNIFIED_ALERT_WRITER_H_

#include <filesystem>
#include <set>
#include <string>

namespace SVF
{

std::string alertSha256(const std::string& text);

class UnifiedAlertWriter
{
public:
    UnifiedAlertWriter(const std::string& alertRoot, const std::string& categoryDir);

    bool write(const std::string& stableIdentity, const std::string& document);
    bool removeStale() const;

private:
    std::filesystem::path directory;
    std::set<std::string> currentFiles;
};

} // namespace SVF

#endif

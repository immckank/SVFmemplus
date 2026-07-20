//===- UnifiedAlertWriter.h -- shared one-file-per-alert persistence -------===//

#ifndef UNIFIED_ALERT_WRITER_H_
#define UNIFIED_ALERT_WRITER_H_

#include <filesystem>
#include <set>
#include <string>

namespace SVF
{

class UnifiedAlertWriter
{
public:
    UnifiedAlertWriter(const std::string& alertRoot, const std::string& warningType);

    /// Persist one warning content object. Identity is the canonical JSON of
    /// {producer,type,content}; mutable downstream fields never affect it.
    bool write(const std::string& producer, const std::string& content);
    bool removeStale() const;

private:
    std::filesystem::path directory;
    std::string warningType;
    std::set<std::string> currentFiles;
};

} // namespace SVF

#endif

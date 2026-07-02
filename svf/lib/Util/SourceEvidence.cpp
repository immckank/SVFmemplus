//===- SourceEvidence.cpp -- shared source-file evidence helpers ----------===//

#include "Util/SourceEvidence.h"
#include "Util/cJSON.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace SVF
{

SourceLocation parseSourceLocation(const std::string& encoded)
{
    SourceLocation location;
    const size_t begin = encoded.find('{');
    const size_t end = encoded.rfind('}');
    const std::string json =
        begin == std::string::npos || end == std::string::npos || end < begin
            ? encoded
            : encoded.substr(begin, end - begin + 1);
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root)
        return location;
    cJSON* file = cJSON_GetObjectItemCaseSensitive(root, "fl");
    if (!cJSON_IsString(file))
        file = cJSON_GetObjectItemCaseSensitive(root, "file");
    if (cJSON_IsString(file) && file->valuestring)
        location.file = file->valuestring;
    cJSON* line = cJSON_GetObjectItemCaseSensitive(root, "ln");
    if (!cJSON_IsNumber(line))
        line = cJSON_GetObjectItemCaseSensitive(root, "line");
    if (cJSON_IsNumber(line))
        location.line = static_cast<int>(line->valuedouble);
    cJSON* column = cJSON_GetObjectItemCaseSensitive(root, "cl");
    if (!cJSON_IsNumber(column))
        column = cJSON_GetObjectItemCaseSensitive(root, "column");
    if (cJSON_IsNumber(column))
        location.column = static_cast<int>(column->valuedouble);
    cJSON_Delete(root);
    return location;
}

std::string readSourceSnippet(const std::string& file, int line,
                              int linesBefore, int linesAfter)
{
    if (file.empty() || line <= 0)
        return "";

    std::vector<std::filesystem::path> candidates{file};
    const char* root = std::getenv("SABER_SOURCE_ROOT");
    if (root == nullptr || !*root)
        root = std::getenv("UAF_SOURCE_ROOT");
    if (root == nullptr || !*root)
        root = std::getenv("UNINIT_SOURCE_ROOT");
    if (root != nullptr && *root)
    {
        const std::filesystem::path rootPath(root);
        candidates.push_back(rootPath / file);
        const std::string marker = rootPath.filename().string() + "/";
        const size_t markerPos = file.find(marker);
        if (!marker.empty() && markerPos != std::string::npos)
            candidates.push_back(rootPath / file.substr(markerPos + marker.size()));
    }

    for (const auto& path : candidates)
    {
        std::ifstream in(path);
        if (!in)
            continue;
        const int from = line - linesBefore > 1 ? line - linesBefore : 1;
        const int to = line + linesAfter;
        std::string result;
        std::string current;
        int number = 0;
        while (std::getline(in, current))
        {
            ++number;
            if (number < from)
                continue;
            if (number > to)
                break;
            result += current + "\n";
        }
        if (!result.empty())
            return result;
    }
    return "";
}

} // namespace SVF

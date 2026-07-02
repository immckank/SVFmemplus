//===- UnifiedAlertWriter.cpp -- shared one-file-per-alert persistence -----===//

#include "Util/UnifiedAlertWriter.h"
#include "Util/cJSON.h"
#include "Util/SVFUtil.h"
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace SVF
{

namespace
{

cJSON* readDocument(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
        return nullptr;
    std::ostringstream text;
    text << in.rdbuf();
    return cJSON_Parse(text.str().c_str());
}

void replaceField(cJSON* document, const char* name, cJSON* value)
{
    if (cJSON_HasObjectItem(document, name))
        cJSON_ReplaceItemInObjectCaseSensitive(document, name, value);
    else
        cJSON_AddItemToObject(document, name, value);
}

} // namespace

std::string alertSha256(const std::string& text)
{
    static constexpr uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };
    std::vector<uint8_t> data(text.begin(), text.end());
    const uint64_t bitLength = static_cast<uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while ((data.size() % 64) != 56)
        data.push_back(0);
    for (int i = 7; i >= 0; --i)
        data.push_back(static_cast<uint8_t>(bitLength >> (i * 8)));
    std::array<uint32_t, 8> h = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    for (size_t offset = 0; offset < data.size(); offset += 64)
    {
        uint32_t w[64] = {};
        for (size_t i = 0; i < 16; ++i)
            w[i] = (uint32_t(data[offset+i*4])<<24) |
                   (uint32_t(data[offset+i*4+1])<<16) |
                   (uint32_t(data[offset+i*4+2])<<8) |
                   uint32_t(data[offset+i*4+3]);
        for (size_t i = 16; i < 64; ++i)
        {
            const uint32_t s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
            const uint32_t s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
            w[i]=w[i-16]+s0+w[i-7]+s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (size_t i=0;i<64;++i)
        {
            const uint32_t s1=rotr(e,6)^rotr(e,11)^rotr(e,25);
            const uint32_t ch=(e&f)^((~e)&g);
            const uint32_t t1=hh+s1+ch+k[i]+w[i];
            const uint32_t s0=rotr(a,2)^rotr(a,13)^rotr(a,22);
            const uint32_t maj=(a&b)^(a&c)^(b&c);
            const uint32_t t2=s0+maj;
            hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;
        h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    std::ostringstream os;
    for (uint32_t word : h)
        os << std::hex << std::setw(8) << std::setfill('0') << word;
    return os.str();
}

UnifiedAlertWriter::UnifiedAlertWriter(const std::string& alertRoot,
                                       const std::string& categoryDir)
    : directory(std::filesystem::path(alertRoot) / categoryDir)
{
}

bool UnifiedAlertWriter::write(const std::string& stableIdentity,
                               const std::string& documentText)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        SVFUtil::errs() << "[Alert] cannot create " << directory.string()
                        << ": " << error.message() << "\n";
        return false;
    }

    const std::string digest = alertSha256(stableIdentity);
    const std::string filename = digest + ".json";
    const std::filesystem::path path = directory / filename;
    cJSON* document = cJSON_Parse(documentText.c_str());
    if (!document)
        return false;

    replaceField(document, "alert_id",
                 cJSON_CreateString(("sha256:" + digest).c_str()));
    if (std::filesystem::exists(path))
    {
        cJSON* old = readDocument(path);
        if (!old)
        {
            cJSON_Delete(document);
            SVFUtil::errs() << "[Alert] cannot read existing document "
                            << path.string() << "\n";
            return false;
        }
        for (const char* field : {"classification", "reason"})
            if (cJSON* value = cJSON_GetObjectItemCaseSensitive(old, field))
                replaceField(document, field, cJSON_Duplicate(value, true));
        cJSON_Delete(old);
    }

    char* rendered = cJSON_Print(document);
    cJSON_Delete(document);
    if (!rendered)
        return false;
    const std::filesystem::path tmp = path.string() + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out)
    {
        cJSON_free(rendered);
        return false;
    }
    out << rendered << "\n";
    cJSON_free(rendered);
    out.close();

    std::filesystem::rename(tmp, path, error);
    if (error)
    {
        error.clear();
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(tmp, path, error);
    }
    if (error)
    {
        SVFUtil::errs() << "[Alert] cannot install " << path.string()
                        << ": " << error.message() << "\n";
        return false;
    }
    currentFiles.insert(filename);
    return true;
}

bool UnifiedAlertWriter::removeStale() const
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
    for (const auto& file : std::filesystem::directory_iterator(directory, error))
    {
        if (error)
            return false;
        if (file.is_regular_file() && file.path().extension() == ".json" &&
            currentFiles.count(file.path().filename().string()) == 0)
            std::filesystem::remove(file.path(), error);
        if (error)
            return false;
    }
    return true;
}

} // namespace SVF

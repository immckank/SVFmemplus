// Faithful clang-16 reproductions of the two reported falconfs defects, used to
// (a) inspect how std::string / strcpy / dirent.d_name lower to LLVM-16 IR and
// (b) validate BOF detection, since the shipped falconfs_ex.bc is LLVM-19
// bitcode that the LLVM-16-based analyzer cannot load.
#include <cstring>
#include <string>
#include <dirent.h>

// ---- BO-1: disk_cache.cpp Walk() lines 109-113 -------------------------------
// char nameBuf[64]; strcpy(nameBuf, f->d_name) guarded by strlen(f->d_name)>32.
// d_name can be up to 255 chars -> copying a >32 (up to 255) name into a 64-byte
// stack buffer can overflow.
extern void sink(const char*);
void Walk(struct dirent* f)
{
    if (strlen(f->d_name) > 32)
    {
        char nameBuf[64];
        strcpy(nameBuf, f->d_name);
        sink(nameBuf);
    }
}

// ---- BO-2: router.cpp GetWorkerConnByPath() lines 95-99 ----------------------
// char shardKey[32]; strcpy(shardKey, filename.data()) guarded by
// filename.size()>32.  size>32 into a 32-byte buffer -> definite overflow.
void GetWorkerConnByPath(const std::string& filename)
{
    if (filename.size() > 32)
    {
        char shardKey[32];
        strcpy(shardKey, filename.data());
        sink(shardKey);
    }
}

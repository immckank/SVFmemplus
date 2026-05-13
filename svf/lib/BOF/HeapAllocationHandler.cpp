#include "BOF/HeapAllocationHandler.h"

using namespace SVF;

/**
 * Define the allocation API map with expected parameter counts.
 * This map is independent of SABER's internal implementation.
 */
const std::map<std::string, u32_t> HeapAllocationHandler::allocApiMap = {
    {"malloc", 1},          {"xmalloc", 1},         {"VOS_MemAlloc", 1},
    {"calloc", 2},          {"SoftBusCalloc", 2},   {"SysCalloc", 2},
    {"realloc", 2},         {"LOS_MemAlloc", 2},    {"LOS_MemRealloc", 3},
    {"alloca", 1},          {"strdup", 1},          {"strndup", 2},
    {"_TIFFmalloc", 1},     {"kmem_cache_alloc", 2}
};

HeapAllocationHandler::HeapAllocationHandler(RangeAnalysis* _ra) : ra(_ra) {}

bool HeapAllocationHandler::isAllocAPI(const std::string& funcName, size_t actualParamCount) const {
    auto it = allocApiMap.find(funcName);
    if (it != allocApiMap.end()) {
        // Validate if the number of parameters matches the pre-defined count
        return it->second == actualParamCount;
    }
    return false;
}

Range HeapAllocationHandler::analyzeAllocSize(const FunObjVar* funObjVar) {
    // Case 1: calloc-style APIs (Size = arg0 * arg1)
    std::string funcName = funObjVar->getName();
    if (funcName == "calloc" || funcName == "SoftBusCalloc" || funcName == "SysCalloc") {
        const ArgValVar* arg0 = funObjVar->getArg(0);
        Range r0 = ra->analyzeVarRange(arg0);

        const ArgValVar* arg1 = funObjVar->getArg(1);
        Range r1 = ra->analyzeVarRange(arg1);

        return Range::mul(r0, r1);
    }
    
    // Case 2: standard malloc-style APIs (Size = arg0)
    // Most allocation APIs put the size in the first argument
    if (funObjVar->arg_size() >= 1) {
        const ArgValVar* size = funObjVar->getArg(0);
        Range sizeRange = ra->analyzeVarRange(size);
        
        // Example usage of Range.add() if needed (e.g., adding a dummy offset)
        // Here we just return the analyzed range directly
        return sizeRange;
    }

    return Range(0);
}
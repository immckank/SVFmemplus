#include "BOF/HeapAllocationHandler.h"

using namespace SVF;

/**
 * Define the allocation API map with the allocation-size expression.
 * This follows SaberCheckerAPI's table-driven API classification, but BOF also
 * needs the argument expression that represents the allocated byte size.
 */
const std::map<std::string, HeapAllocationHandler::AllocSizeSpec> HeapAllocationHandler::allocApiMap = {
    {"alloc", {1, AS_ARG, 0, 0}},
    {"alloc_check", {1, AS_ARG, 0, 0}},
    {"alloc_clear", {1, AS_ARG, 0, 0}},
    {"calloc", {2, AS_MUL_ARGS, 0, 1}},
    {"lalloc", {1, AS_ARG, 0, 0}},
    {"lalloc_clear", {1, AS_ARG, 0, 0}},
    {"malloc", {1, AS_ARG, 0, 0}},
    {"safe_calloc", {2, AS_MUL_ARGS, 0, 1}},
    {"safe_malloc", {1, AS_ARG, 0, 0}},
    {"safecalloc", {2, AS_MUL_ARGS, 0, 1}},
    {"safemalloc", {1, AS_ARG, 0, 0}},
    {"safexcalloc", {2, AS_MUL_ARGS, 0, 1}},
    {"safexmalloc", {1, AS_ARG, 0, 0}},
    {"savealloc", {1, AS_ARG, 0, 0}},
    {"xalloc", {1, AS_ARG, 0, 0}},
    {"xcalloc", {2, AS_MUL_ARGS, 0, 1}},
    {"xmalloc", {1, AS_ARG, 0, 0}},
    {"SoftBusMalloc", {1, AS_ARG, 0, 0}},
    {"SoftBusCalloc", {2, AS_MUL_ARGS, 0, 1}},
    {"SysMalloc", {1, AS_ARG, 0, 0}},
    {"SysCalloc", {2, AS_MUL_ARGS, 0, 1}},
    {"FillpMemAlloc", {1, AS_ARG, 0, 0}},
    {"FillpMemCalloc", {2, AS_MUL_ARGS, 0, 1}},
    {"OhosMalloc", {1, AS_ARG, 0, 0}},
    {"VOS_MemAlloc", {1, AS_ARG, 0, 0}},
    {"_TIFFmalloc", {1, AS_ARG, 0, 0}},
    {"__kmalloc", {1, AS_ARG, 0, 0}},
    {"kmalloc_large", {1, AS_ARG, 0, 0}},
    {"kmalloc_trace", {1, AS_ARG, 0, 0}},
    {"realloc", {2, AS_ARG, 1, 0}},
    {"strndup", {2, AS_ARG, 1, 0}},
    {"LOS_MemAlloc", {2, AS_ARG, 1, 0}},
    {"LOS_MemAllocAlign", {3, AS_ARG, 1, 0}},
    {"LOS_MemRealloc", {3, AS_ARG, 2, 0}},
    {"alloca", {1, AS_ARG, 0, 0}},

    {"strdup", {0, AS_UNKNOWN, 0, 0}},
    {"realpath", {0, AS_UNKNOWN, 0, 0}},
    {"jpeg_alloc_huff_table", {0, AS_UNKNOWN, 0, 0}},
    {"jpeg_alloc_quant_table", {0, AS_UNKNOWN, 0, 0}},
    {"png_create_info_struct", {0, AS_UNKNOWN, 0, 0}},
    {"png_create_write_struct", {0, AS_UNKNOWN, 0, 0}},
    {"SSL_CTX_new", {0, AS_UNKNOWN, 0, 0}},
    {"SSL_new", {0, AS_UNKNOWN, 0, 0}},
    {"kmem_cache_alloc", {0, AS_UNKNOWN, 0, 0}},
    {"kmem_cache_zalloc", {0, AS_UNKNOWN, 0, 0}}
};

HeapAllocationHandler::HeapAllocationHandler(RangeAnalysis* _ra) : ra(_ra) {}

bool HeapAllocationHandler::isAllocAPI(const std::string& funcName, size_t actualParamCount) const {
    auto it = allocApiMap.find(funcName);
    return it != allocApiMap.end() && actualParamCount >= it->second.requiredArgs;
}

Range HeapAllocationHandler::analyzeAllocSize(const FunObjVar* funObjVar) {
    if(funObjVar == nullptr)
        return Range::BOTTOM;

    auto it = allocApiMap.find(funObjVar->getName());
    if(it == allocApiMap.end() || funObjVar->arg_size() < it->second.requiredArgs)
        return Range::BOTTOM;

    return analyzeBySpec(it->second, funObjVar);
}

Range HeapAllocationHandler::analyzeAllocSize(const CallICFGNode* callInst) {
    if(callInst == nullptr || callInst->isIndirectCall())
        return Range::BOTTOM;

    const FunObjVar* funObjVar = callInst->getCalledFunction();
    if(funObjVar == nullptr)
        return Range::BOTTOM;

    std::string funcName = funObjVar->getName();
    auto it = allocApiMap.find(funcName);
    if(it == allocApiMap.end() || callInst->arg_size() < it->second.requiredArgs)
        return Range::BOTTOM;

    return analyzeBySpec(it->second, callInst);
}

Range HeapAllocationHandler::analyzeArgRange(const CallICFGNode* callInst, u32_t idx) const {
    if(callInst == nullptr || idx >= callInst->arg_size())
        return Range::BOTTOM;
    return ra->analyzeVarRange(callInst->getArgument(idx));
}

Range HeapAllocationHandler::analyzeArgRange(const FunObjVar* funObjVar, u32_t idx) const {
    if(funObjVar == nullptr || idx >= funObjVar->arg_size())
        return Range::BOTTOM;
    return ra->analyzeVarRange(funObjVar->getArg(idx));
}

Range HeapAllocationHandler::analyzeBySpec(const AllocSizeSpec& spec, const CallICFGNode* callInst) const {
    if(spec.kind == AS_ARG)
        return analyzeArgRange(callInst, spec.arg0);
    if(spec.kind == AS_MUL_ARGS)
        return Range::mul(analyzeArgRange(callInst, spec.arg0), analyzeArgRange(callInst, spec.arg1));
    return Range::TOP;
}

Range HeapAllocationHandler::analyzeBySpec(const AllocSizeSpec& spec, const FunObjVar* funObjVar) const {
    if(spec.kind == AS_ARG)
        return analyzeArgRange(funObjVar, spec.arg0);
    if(spec.kind == AS_MUL_ARGS)
        return Range::mul(analyzeArgRange(funObjVar, spec.arg0), analyzeArgRange(funObjVar, spec.arg1));
    return Range::TOP;
}

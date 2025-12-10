# SVFmem+
To provide detection support for use-after-free (UAF), undefined usages, and array out-of-bounds within the SABER framework of SVF (a static analysis tool).

## 模块功能

本模块将提供针对编译完成后的c语言程序(预期为.bc文件)，执行静态分析流程并产出静态分析报告。

## 环境配置

请参考dockerfile中给出的环境要求，在构建完成镜像后：

```bash
cd SVFmenplus && ./build.sh
# 本命令将完成静态分析器的编译构建
# 如果已经完成了编译构建情使用 ./setup.sh直接导出环境变量即可
```

因为要下载llvm，时间可能比较久

## 静态分析报告生成

```bash
saber <option> bc_file.bc
```

option就是指定分析的缺陷类型，包含：
    - -leak: NeverFree、PartialLeak
    - -dfree: DoubleFree
    - -uaf: UseAfterFree    
    
构建后如果找不到libz3.so.4请执行
    
```bash
ln -s /src/SVFmemplus/z3.obj/bin/libz3.so /usr/lib/libz3.so.4
```

## 其他说明

1. 入口函数可以灵活调整，对于从入口函数开始分析但无法抵达的函数，指针分析不会对其进行建模。

```c
// svf/lib/Util/SVFUtil.cpp L442-445
bool SVFUtil::isProgEntryFunction(const FunObjVar* funObjVar)
{
    return funObjVar && funObjVar->getName() == "main";
}
```

2. 用户自定义的内存申请函数与内存释放函数一般可以直接识别，如果未能识别可以自行添加与完善。

```c
// svf/lib/SABER/SaberCheckerAPI.cpp L51-L138
static const ei_pair ei_pairs[]=
{
    {"alloc", SaberCheckerAPI::CK_ALLOC},
    {"alloc_check", SaberCheckerAPI::CK_ALLOC},
    ...
```

3. 对于大型项目可以产出大量警报信息，工具默认使用标准输出流输出警报信息，可以使用输出重定向来使文件捕获输出。

```bash
// 示例
saber -leak bc_example.bc >> example.txt
```
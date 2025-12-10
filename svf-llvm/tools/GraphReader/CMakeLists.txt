# 定义可执行文件的名称为 "graph-reader"，源文件为 GraphReader.cpp
# ... 其他CMake命令 ...

add_executable(graph-reader
    GraphReader.cpp
    GraphReaderUtil.cpp
    GraphReaderSVFGBuilder.cpp
    FunctionQuery.cpp
    PathQuery.cpp
)


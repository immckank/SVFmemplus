add_executable(svf-al-export
    ActiveLearningExport.cpp
    ../GraphReader/GraphReaderSVFGBuilder.cpp
)

target_include_directories(svf-al-export PRIVATE ../GraphReader)

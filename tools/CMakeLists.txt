# 测量工具。不进入 ctest（无 add_test）；各工具的方法与复现命令见其目录内
# README。fft_bench（M4-01/DEC-012 决策取证）为独立编译工具，无 CMake 目标。

add_executable(workflow_bench workflow_bench/workflow_bench.cpp)
target_compile_features(workflow_bench PRIVATE cxx_std_20)
target_link_libraries(workflow_bench PRIVATE rin::workflow)

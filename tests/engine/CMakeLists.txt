# 引擎级交互边界测试：第一个链接 CoronaEngine 的目标。
# SharedDataHub 的命令/快照语义只能在引擎库里跑到，kernel 测试覆盖不到。
add_executable(corona_interaction_boundary_tests
    test_interaction_boundary.cpp
)
target_link_libraries(corona_interaction_boundary_tests PRIVATE CoronaEngine)
add_test(NAME InteractionBoundaryTests
         COMMAND corona_interaction_boundary_tests)

# 引擎级交互边界测试：第一个链接 CoronaEngine 的目标。
# SharedDataHub 的命令/快照语义只能在引擎库里跑到，kernel 测试覆盖不到。
add_executable(corona_interaction_boundary_tests
    test_interaction_boundary.cpp
)
target_link_libraries(corona_interaction_boundary_tests PRIVATE CoronaEngine)
add_test(NAME InteractionBoundaryTests
         COMMAND corona_interaction_boundary_tests)

add_executable(corona_optics_command_tests test_optics_commands.cpp)
target_link_libraries(corona_optics_command_tests PRIVATE CoronaEngine)
add_test(NAME OpticsCommandTests COMMAND corona_optics_command_tests)

# 结构命令通道的结算/重入/超时/停机契约（spec §5.1）。用假拥有者线程注入故障，
# 因此不需要 GPU 或真 DAG；TIMEOUT 是"任何等待都必须有界"的最后一道保险。
add_executable(corona_structural_command_tests test_structural_commands.cpp)
target_link_libraries(corona_structural_command_tests PRIVATE CoronaEngine)
add_test(NAME StructuralCommandTests COMMAND corona_structural_command_tests)
set_tests_properties(StructuralCommandTests PROPERTIES TIMEOUT 120)

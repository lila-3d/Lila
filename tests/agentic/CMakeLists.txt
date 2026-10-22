# The MCP server over real HTTP, the agent guides and prompts, workspace integration files, and offscreen captures.
add_executable(lila_agentic_tests main.cpp test_mcp.cpp test_integration.cpp)
target_link_libraries(lila_agentic_tests PRIVATE lila_ui doctest::doctest)
add_test(NAME lila_agentic_tests COMMAND lila_agentic_tests)

# The modeling guide's workflow run headless: the example dog must come out as one closed, manifold, symmetric shell.
add_test(NAME lila_agent_modeling COMMAND lilabatch --batch ${CMAKE_CURRENT_SOURCE_DIR}/agent_modeling.py)
set_tests_properties(lila_agent_modeling PROPERTIES ENVIRONMENT "LILA_CONFIG_DIR=${CMAKE_CURRENT_BINARY_DIR}/agent_modeling_cfg")

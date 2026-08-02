set(mcp_http_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_mcp_http STATIC mcp_http_server.cpp)
target_include_directories(palmier_mcp_http PUBLIC "${mcp_http_include_dir}")
target_compile_features(palmier_mcp_http PUBLIC cxx_std_20)
target_link_libraries(palmier_mcp_http PUBLIC palmier_project_session Ws2_32)

add_executable(palmier_windows_mcp app/main.cpp)
target_link_libraries(palmier_windows_mcp PRIVATE palmier_mcp_http Ole32)

foreach(target palmier_mcp_http palmier_windows_mcp)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
endforeach()

if(BUILD_TESTING)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME mcp.http_split_undo_e2e
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_http_e2e.py"
            --server "$<TARGET_FILE:palmier_windows_mcp>"
            --fixture "${PROJECT_SOURCE_DIR}/fixtures/contracts/projects/current-multitimeline.palmier"
            --contract "${PROJECT_SOURCE_DIR}/contracts/mcp/v1/windows-technical-mvp.json"
            --port 19789
    )
    set_tests_properties(
        mcp.http_split_undo_e2e
        PROPERTIES
            RESOURCE_LOCK palmier_mcp_port_19789
            RUN_SERIAL TRUE
            TIMEOUT 45
            LABELS "mcp;e2e"
            ENVIRONMENT "PYTHONUTF8=1"
    )
endif()

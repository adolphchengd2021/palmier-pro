set(project_runtime_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_project_runtime STATIC project_runtime.cpp)
target_include_directories(palmier_project_runtime PUBLIC "${project_runtime_include_dir}")
target_compile_features(palmier_project_runtime PUBLIC cxx_std_20)
target_link_libraries(palmier_project_runtime PUBLIC palmier_project_session)
target_compile_options(
    palmier_project_runtime
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)

if(BUILD_TESTING)
    add_executable(palmier_project_runtime_tests tests/project_runtime_tests.cpp)
    target_link_libraries(palmier_project_runtime_tests PRIVATE palmier_project_runtime)
    target_compile_options(
        palmier_project_runtime_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(NAME project_runtime.serial_owner COMMAND palmier_project_runtime_tests)
endif()

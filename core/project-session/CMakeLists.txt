set(project_session_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_project_session STATIC project_session.cpp)
target_include_directories(palmier_project_session PUBLIC "${project_session_include_dir}")
target_compile_features(palmier_project_session PUBLIC cxx_std_20)
target_link_libraries(palmier_project_session PUBLIC palmier_project_reader)
target_compile_options(
    palmier_project_session
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)

if(BUILD_TESTING)
    add_executable(
        palmier_project_session_tests
        tests/project_session_tests.cpp
    )
    target_link_libraries(
        palmier_project_session_tests
        PRIVATE palmier_project_session
    )
    target_compile_options(
        palmier_project_session_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME project_session.split_undo
        COMMAND palmier_project_session_tests "${PROJECT_SOURCE_DIR}"
    )
endif()

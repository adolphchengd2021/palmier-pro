set(project_package_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(
    palmier_windows_project_package
    STATIC
        include/palmier/project/project_recovery_journal.hpp
        include/palmier/project/project_package_service.hpp
        include/palmier/project/windows_project_package_writer.hpp
        project_recovery_journal.cpp
        project_package_service.cpp
        windows_project_package_writer.cpp
)
target_include_directories(
    palmier_windows_project_package
    PUBLIC "${project_package_include_dir}"
)
target_compile_features(palmier_windows_project_package PUBLIC cxx_std_20)
target_link_libraries(
    palmier_windows_project_package
    PUBLIC palmier_project_runtime palmier_project_reader
    PRIVATE bcrypt shell32 ole32
)
target_compile_options(
    palmier_windows_project_package
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)

if(BUILD_TESTING)
    add_executable(
        palmier_windows_project_package_tests
        tests/windows_project_package_writer_tests.cpp
    )
    target_include_directories(
        palmier_windows_project_package_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        palmier_windows_project_package_tests
        PRIVATE palmier_windows_project_package
    )
    target_compile_definitions(
        palmier_windows_project_package_tests
        PRIVATE PALMIER_REPOSITORY_ROOT="${PROJECT_SOURCE_DIR}"
    )
    target_compile_options(
        palmier_windows_project_package_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME project_package.write_restart_round_trip
        COMMAND palmier_windows_project_package_tests
    )

    add_executable(
        palmier_project_recovery_journal_tests
        tests/project_recovery_journal_tests.cpp
    )
    target_include_directories(
        palmier_project_recovery_journal_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        palmier_project_recovery_journal_tests
        PRIVATE palmier_windows_project_package
    )
    target_compile_options(
        palmier_project_recovery_journal_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME project_package.recovery_journal
        COMMAND palmier_project_recovery_journal_tests
    )
    set_tests_properties(project_package.recovery_journal PROPERTIES TIMEOUT 60)
endif()

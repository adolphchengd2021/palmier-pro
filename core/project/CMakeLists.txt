set(project_core_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_json STATIC
    serialization/json_document.cpp
    serialization/top_level_json.cpp
)
target_include_directories(palmier_json PUBLIC "${project_core_include_dir}")
target_compile_features(palmier_json PUBLIC cxx_std_20)

add_library(palmier_project_reader STATIC
    serialization/media_manifest_reader.cpp
    serialization/project_media_resolver.cpp
    serialization/project_package_reader.cpp
    serialization/project_reader.cpp
)
target_include_directories(palmier_project_reader PUBLIC "${project_core_include_dir}")
target_compile_features(palmier_project_reader PUBLIC cxx_std_20)
target_link_libraries(palmier_project_reader PUBLIC palmier_json)

foreach(target palmier_json palmier_project_reader)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
endforeach()

if(BUILD_TESTING)
    add_executable(
        palmier_project_package_reader_tests
        tests/project_package_reader_tests.cpp
    )
    target_include_directories(
        palmier_project_package_reader_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/serialization"
    )
    target_link_libraries(palmier_project_package_reader_tests PRIVATE palmier_project_reader)
    target_compile_options(
        palmier_project_package_reader_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME project_reader.package_api
        COMMAND palmier_project_package_reader_tests "${PROJECT_SOURCE_DIR}"
    )

    add_executable(palmier_project_reader_tests tests/project_reader_tests.cpp)
    target_link_libraries(palmier_project_reader_tests PRIVATE palmier_project_reader)
    target_compile_options(
        palmier_project_reader_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME project_reader.direct_api
        COMMAND palmier_project_reader_tests "${PROJECT_SOURCE_DIR}"
    )
endif()

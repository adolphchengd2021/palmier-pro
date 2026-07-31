find_package(
    Qt6 6.10.3 EXACT REQUIRED
    COMPONENTS Core Gui Qml Quick QuickControls2 QuickDialogs2 Concurrent
)

qt_standard_project_setup(REQUIRES 6.10.3)

set(app_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_qt_shell_core STATIC
    include/palmier/windows/project_load_coordinator.hpp
    include/palmier/windows/project_projection_loader.hpp
    include/palmier/windows/read_only_timeline_model.hpp
    src/project_load_coordinator.cpp
    src/project_projection_loader.cpp
    src/read_only_timeline_model.cpp
)
target_include_directories(palmier_qt_shell_core PUBLIC "${app_include_dir}")
target_link_libraries(
    palmier_qt_shell_core
    PUBLIC Qt6::Core Qt6::Concurrent palmier_project_reader
)
target_compile_features(palmier_qt_shell_core PUBLIC cxx_std_20)

qt_add_executable(palmier_qt_shell app/main.cpp)
set_source_files_properties(qml/AppTheme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(palmier_qt_shell
    URI PalmierPro.Windows
    VERSION 1.0
    QML_FILES qml/Main.qml qml/AppTheme.qml
)
target_link_libraries(
    palmier_qt_shell
    PRIVATE
        palmier_qt_shell_core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickDialogs2
)

foreach(target palmier_qt_shell_core palmier_qt_shell)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
endforeach()

if(BUILD_TESTING)
    find_package(Qt6 6.10.3 EXACT REQUIRED COMPONENTS Test)
    qt_add_executable(palmier_qt_shell_tests
        tests/qt_shell_tests.cpp
    )
    target_link_libraries(
        palmier_qt_shell_tests
        PRIVATE
            palmier_qt_shell_core
            Qt6::Gui
            Qt6::Qml
            Qt6::Quick
            Qt6::QuickControls2
            Qt6::QuickDialogs2
            Qt6::Test
    )
    target_compile_definitions(
        palmier_qt_shell_tests
        PRIVATE
            PALMIER_REPOSITORY_ROOT="${PROJECT_SOURCE_DIR}"
            PALMIER_QML_MAIN_FILE="${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml"
    )
    target_compile_options(
        palmier_qt_shell_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_dependencies(palmier_qt_shell_tests palmier_qt_shell)
    add_test(NAME qt_shell.reader_model_qml COMMAND palmier_qt_shell_tests)
    set_tests_properties(qt_shell.reader_model_qml PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME qt_shell.module_smoke COMMAND palmier_qt_shell --smoke-test)
    set_tests_properties(qt_shell.module_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

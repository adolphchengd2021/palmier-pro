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
    function(add_palmier_qt_shell_test test_name test_function)
        add_test(
            NAME qt_shell.${test_name}
            COMMAND palmier_qt_shell_tests ${test_function} -v1
        )
        set_tests_properties(
            qt_shell.${test_name}
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30
        )
    endfunction()
    add_palmier_qt_shell_test(reader_maps_current_project readerMapsCurrentProject)
    add_palmier_qt_shell_test(model_publishes_track_layout modelPublishesReadOnlyTrackLayout)
    add_palmier_qt_shell_test(failure_preserves_model failurePreservesPreviousModel)
    add_palmier_qt_shell_test(stale_generation_is_rejected staleGenerationCannotReplaceNewerProject)
    add_palmier_qt_shell_test(consecutive_opens_keep_latest consecutiveOpensKeepOnlyLatestPendingRequest)
    add_palmier_qt_shell_test(cancellation_reaches_reader cancellationReachesReader)
    add_palmier_qt_shell_test(late_cancellation_rejects_result cancellationAfterWorkBeforeCommitRejectsResult)
    add_palmier_qt_shell_test(shutdown_waits_for_worker shutdownWaitsForAdmittedWorker)
    add_palmier_qt_shell_test(unsafe_clip_is_skipped unsafeClipIsSkippedWithoutRejectingProject)
    add_palmier_qt_shell_test(dense_timeline_is_rejected denseTimelineIsExplicitlyRejected)
    add_palmier_qt_shell_test(structured_errors_preserve_details structuredErrorsPreserveStableDetails)
    add_palmier_qt_shell_test(diagnostics_publish_warnings diagnosticsProduceLoadedWithWarnings)
    add_palmier_qt_shell_test(qml_loads_offscreen qmlLoadsOffscreen)
    add_palmier_qt_shell_test(qml_close_waits_for_worker qmlCloseWaitsForActiveWorker)
    add_test(NAME qt_shell.module_smoke COMMAND palmier_qt_shell --smoke-test)
    set_tests_properties(qt_shell.module_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

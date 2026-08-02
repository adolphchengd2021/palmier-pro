find_package(
    Qt6 6.10.3 EXACT REQUIRED
    COMPONENTS Core Gui Qml Quick QuickControls2 QuickDialogs2 Concurrent
)

qt_standard_project_setup(REQUIRES 6.10.3)

set(app_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_qt_shell_core STATIC
    include/palmier/windows/project_load_coordinator.hpp
    include/palmier/windows/project_editing_controller.hpp
    include/palmier/windows/project_export_controller.hpp
    include/palmier/windows/project_persistence_controller.hpp
    include/palmier/windows/project_projection_loader.hpp
    include/palmier/windows/project_runtime_mailbox.hpp
    include/palmier/windows/project_runtime_projection_bridge.hpp
    include/palmier/windows/preview_source.hpp
    include/palmier/windows/read_only_timeline_model.hpp
    src/project_load_coordinator.cpp
    src/project_editing_controller.cpp
    src/project_export_controller.cpp
    src/project_persistence_controller.cpp
    src/project_projection_loader.cpp
    src/project_runtime_mailbox.cpp
    src/project_runtime_projection_bridge.cpp
    src/read_only_timeline_model.cpp
)
target_include_directories(palmier_qt_shell_core PUBLIC "${app_include_dir}")
target_link_libraries(
    palmier_qt_shell_core
    PUBLIC
        Qt6::Core
        Qt6::Concurrent
        palmier_project_reader
        palmier_project_render
        palmier_project_runtime
        palmier_export_ffmpeg
        palmier_windows_project_package
)
target_compile_features(palmier_qt_shell_core PUBLIC cxx_std_20)

add_library(palmier_qt_preview STATIC
    include/palmier/windows/preview_presentation_controller.hpp
    src/preview_presentation_controller.cpp
)
target_include_directories(palmier_qt_preview PUBLIC "${app_include_dir}")
target_link_libraries(
    palmier_qt_preview
    PUBLIC
        Qt6::Core
        Qt6::Gui
        palmier_preview_session
)
target_compile_features(palmier_qt_preview PUBLIC cxx_std_20)

qt_add_executable(palmier_qt_shell app/main.cpp)
install(
    TARGETS palmier_qt_shell
    RUNTIME DESTINATION .
    COMPONENT prototype
)
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
        palmier_qt_preview
        palmier_mcp_http
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickDialogs2
)

foreach(target palmier_qt_shell_core palmier_qt_preview palmier_qt_shell)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
endforeach()

if(BUILD_TESTING)
    find_package(Qt6 6.10.3 EXACT REQUIRED COMPONENTS Test)
    qt_add_executable(palmier_qt_shell_tests
        tests/qt_shell_tests.cpp
    )
    set_target_properties(palmier_qt_shell_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(
        palmier_qt_shell_tests
        PRIVATE
            palmier_qt_shell_core
            palmier_qt_preview
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
        set(test_log "${CMAKE_CURRENT_BINARY_DIR}/qt-test-${test_name}.txt")
        add_test(
            NAME qt_shell.${test_name}
            COMMAND palmier_qt_shell_tests ${test_function} -v1 -o "${test_log},txt"
        )
        set_tests_properties(
            qt_shell.${test_name}
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30
        )
    endfunction()
    add_palmier_qt_shell_test(reader_maps_current_project readerMapsCurrentProject)
    add_palmier_qt_shell_test(
        editing_controller_split_undo
        editingControllerSplitsAndUndoesByStableId
    )
    add_palmier_qt_shell_test(
        editing_controller_move_undo
        editingControllerMovesByStableIdAndPreservesNoOpHistory
    )
    add_palmier_qt_shell_test(
        editing_controller_remove_undo
        editingControllerRemovesByStableIdAndUndoes
    )
    add_palmier_qt_shell_test(
        persistence_save_waits_for_shutdown
        persistenceSaveRunsOffGuiAndShutdownWaits
    )
    add_palmier_qt_shell_test(
        persistence_shutdown_refreshes_dirty
        persistenceShutdownRefreshesAuthoritativeDirtyState
    )
    add_palmier_qt_shell_test(
        persistence_failure_preserves_dirty
        persistenceFailurePreservesDirtyState
    )
    add_palmier_qt_shell_test(
        persistence_cancellation_reaches_save
        persistenceCancellationReachesAdmittedSave
    )
    add_palmier_qt_shell_test(
        persistence_committed_warning
        persistenceCommittedWarningRemainsObservable
    )
    add_palmier_qt_shell_test(
        persistence_teardown_retains_runtime
        persistenceWorkerRetainsRuntimeAfterControllerTeardown
    )
    add_palmier_qt_shell_test(
        dirty_runtime_refuses_replacement
        dirtyRuntimeRefusesProjectReplacement
    )
    add_palmier_qt_shell_test(
        runtime_mailbox_identity
        runtimeMailboxPublishesUndoAndPersistenceIdentity
    )
    add_palmier_qt_shell_test(
        runtime_mutation_refreshes_projection
        runtimeMutationRefreshesQtProjectionAndInvalidatesPreview
    )
    add_palmier_qt_shell_test(
        late_cancel_preserves_runtime_install
        cancellationAfterRuntimeInstallCannotRollbackCommit
    )
    add_palmier_qt_shell_test(
        persistence_retags_inflight_projection
        persistencePublicationRetagsInFlightProjection
    )
    add_palmier_qt_shell_test(
        superseded_install_waits_for_latest_load
        supersededInstalledProjectWaitsForLatestLoadOutcome
    )
    add_palmier_qt_shell_test(
        stable_preview_candidate_uses_persisted_ids
        stablePreviewCandidateUsesPersistedIds
    )
    add_palmier_qt_shell_test(
        save_as_rebases_only_project_preview_sources
        saveAsRebasesOnlyProjectPreviewSources
    )
    add_palmier_qt_shell_test(
        unsupported_visual_properties_are_not_silently_dropped
        unsupportedVisualPropertiesAreNotSilentlyDropped
    )
    add_palmier_qt_shell_test(
        malformed_earlier_visual_cannot_be_skipped
        malformedEarlierVisualCannotBeSkippedForLaterCandidate
    )
    add_palmier_qt_shell_test(
        trimmed_timing_remains_first_candidate
        trimmedTimingRemainsTheFirstPreviewCandidate
    )
    add_palmier_qt_shell_test(
        overlapping_visual_layer_is_explicitly_refused
        overlappingVisualLayerIsExplicitlyRefused
    )
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

    qt_add_executable(
        palmier_project_export_controller_tests
        tests/project_export_controller_tests.cpp
    )
    set_target_properties(
        palmier_project_export_controller_tests
        PROPERTIES WIN32_EXECUTABLE FALSE
    )
    target_link_libraries(
        palmier_project_export_controller_tests
        PRIVATE palmier_qt_shell_core Qt6::Test
    )
    target_compile_options(
        palmier_project_export_controller_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME qt_shell.export_controller
        COMMAND palmier_project_export_controller_tests -v1
    )
    set_tests_properties(
        qt_shell.export_controller
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30
    )

    qt_add_executable(palmier_qt_preview_tests tests/qt_preview_tests.cpp)
    set_target_properties(palmier_qt_preview_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(
        palmier_qt_preview_tests
        PRIVATE
            palmier_qt_preview
            palmier_qt_shell_core
            Qt6::Concurrent
            Qt6::Gui
            Qt6::Qml
            Qt6::Quick
            Qt6::QuickControls2
            Qt6::QuickDialogs2
            Qt6::Test
    )
    target_compile_definitions(
        palmier_qt_preview_tests
        PRIVATE PALMIER_QML_MAIN_FILE="${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml"
    )
    target_include_directories(
        palmier_qt_preview_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../media-ffmpeg/tests"
    )
    target_compile_options(
        palmier_qt_preview_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    function(add_palmier_qt_preview_test test_name test_function)
        set(test_log "${CMAKE_CURRENT_BINARY_DIR}/qt-preview-test-${test_name}.txt")
        add_test(
            NAME qt_preview.${test_name}
            COMMAND palmier_qt_preview_tests ${test_function} -v1 -o "${test_log},txt"
        )
        set_tests_properties(
            qt_preview.${test_name}
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows" TIMEOUT 30
        )
    endfunction()
    add_palmier_qt_preview_test(
        native_child_background_owner
        nativeChildUsesOneBackgroundSessionOwner
    )
    add_palmier_qt_preview_test(
        resize_burst_coalesces
        resizeBurstKeepsOnlyLatestPhysicalSize
    )
    add_palmier_qt_preview_test(
        project_candidate_ticks_to_completion
        projectCandidateTicksOnceAndStopsAtCompletion
    )
    add_palmier_qt_preview_test(
        project_candidate_waits_for_attach
        projectCandidateWaitsForActiveSurfaceAttach
    )
    add_palmier_qt_preview_test(
        resize_during_tick_resumes_cadence
        resizeDuringGatedTickResumesBoundedCadence
    )
    add_palmier_qt_preview_test(
        pause_resume_preserves_generation
        pauseCancelsActiveTickAndResumeKeepsTheGeneration
    )
    add_palmier_qt_preview_test(
        stale_tick_cancels_without_retry
        staleTickCancelsWithoutRetryLoop
    )
    add_palmier_qt_preview_test(
        replacement_cancels_gated_tick
        replacementCancelsGatedTickBeforePublishingOffline
    )
    add_palmier_qt_preview_test(
        runtime_revision_invalidates_preview
        sameProjectRevisionInvalidatesActivePreview
    )
    add_palmier_qt_preview_test(
        resolved_invalidation_publishes_same_revision
        resolvedInvalidationPublishesSameProjectRevision
    )
    add_palmier_qt_preview_test(
        shutdown_cancels_gated_tick
        shutdownCancelsGatedTickBeforeSessionDestruction
    )
    add_palmier_qt_preview_test(
        warp_child_surface_lifecycle
        warpChildSurfaceLifecycle
    )
    add_palmier_qt_preview_test(
        project_package_warp_presentation
        projectPackageDrivesWarpPresentationOrReportsAudioUnavailable
    )
    set_tests_properties(
        qt_preview.project_package_warp_presentation
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 60
    )
    add_palmier_qt_preview_test(
        qml_close_waits_for_preview_release
        qmlCloseWaitsForPreviewSessionRelease
    )
    add_palmier_qt_preview_test(
        unexpected_teardown_retires_window
        unexpectedTeardownRetiresWindowUntilSessionDestruction
    )
    add_palmier_qt_preview_test(
        shutdown_during_ready_signal
        shutdownDuringReadySignalDoesNotRestoreReadyState
    )
    add_palmier_qt_preview_test(
        shutdown_failure_signal_reentry
        shutdownFailureSignalReentryStillNotifiesDrain
    )
    add_test(
        NAME qt_preview.programmatic_quit_drains
        COMMAND palmier_qt_shell --quit-smoke-test
    )
    set_tests_properties(
        qt_preview.programmatic_quit_drains
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows" TIMEOUT 30
    )
endif()

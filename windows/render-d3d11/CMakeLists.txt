set(d3d11_render_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(
    palmier_render_d3d11 STATIC
    d3d11_preview_surface.cpp
    d3d11_warp_renderer.cpp
)
target_include_directories(
    palmier_render_d3d11
    PUBLIC "${d3d11_render_include_dir}"
)
target_compile_features(palmier_render_d3d11 PUBLIC cxx_std_20)
target_compile_definitions(
    palmier_render_d3d11
    PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
)
target_compile_options(
    palmier_render_d3d11
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)
target_link_libraries(
    palmier_render_d3d11
    PUBLIC palmier_render
    PRIVATE d3d11 d3dcompiler dxgi user32
)

if(BUILD_TESTING)
    add_executable(
        palmier_d3d11_warp_tests
        tests/warp_render_tests.cpp
    )
    target_link_libraries(
        palmier_d3d11_warp_tests
        PRIVATE palmier_render_d3d11
    )
    target_compile_options(
        palmier_d3d11_warp_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(NAME render_d3d11.warp_parity COMMAND palmier_d3d11_warp_tests)
    set_tests_properties(
        render_d3d11.warp_parity
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 60
    )

    add_executable(
        palmier_d3d11_preview_surface_smoke
        tests/preview_surface_smoke.cpp
    )
    target_link_libraries(
        palmier_d3d11_preview_surface_smoke
        PRIVATE palmier_render_d3d11
    )
    target_compile_options(
        palmier_d3d11_preview_surface_smoke
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    target_include_directories(
        palmier_d3d11_preview_surface_smoke
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
        NAME render_d3d11.hidden_hwnd_warp_present_smoke
        COMMAND palmier_d3d11_preview_surface_smoke
    )
    set_tests_properties(
        render_d3d11.hidden_hwnd_warp_present_smoke
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 30
    )
endif()

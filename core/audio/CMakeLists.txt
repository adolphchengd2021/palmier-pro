set(audio_core_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_audio_core INTERFACE)
target_include_directories(
    palmier_audio_core
    INTERFACE "${audio_core_include_dir}"
)
target_compile_features(palmier_audio_core INTERFACE cxx_std_20)

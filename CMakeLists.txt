cmake_minimum_required(VERSION 3.24)
project(DLSSCamDemo VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DLSSCamDemo currently targets Windows x64 / D3D12 only.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "Use Visual Studio 2022 / MSVC for this build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(DLSS_SDK "${CMAKE_SOURCE_DIR}/external/DLSS" CACHE PATH "Path to NVIDIA/DLSS SDK checkout")
if(NOT EXISTS "${DLSS_SDK}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "NVIDIA DLSS SDK not found at ${DLSS_SDK}. Run build_windows.bat; it clones the official NVIDIA/DLSS repository automatically.")
endif()

set(DLSS_LIB_DIR "${DLSS_SDK}/lib/Windows_x86_64/x64")
set(DLSS_RUNTIME_REL "${DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
set(DLSS_RUNTIME_DEV "${DLSS_SDK}/lib/Windows_x86_64/dev/nvngx_dlss.dll")

add_executable(DLSSCamDemo WIN32
    src/main.cpp
    src/CameraSource.cpp
    src/CameraSource.h
    src/DLSSBackend.cpp
    src/DLSSBackend.h
    src/DLSSNRBackend.cpp
    src/DLSSNRBackend.h
    src/D3D12Renderer.cpp
    src/D3D12Renderer.h
    src/TemporalGuides.cpp
    src/TemporalGuides.h
    src/PersonSegmenter.cpp
    src/PersonSegmenter.h
    src/Log.h
)

target_compile_definitions(DLSSCamDemo PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
target_compile_options(DLSSCamDemo PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus /wd4819)
set(ORT_DIR "${CMAKE_SOURCE_DIR}/external/onnxruntime" CACHE PATH "ONNX Runtime (win-x64) package")
if(NOT EXISTS "${ORT_DIR}/include/onnxruntime_cxx_api.h")
    message(FATAL_ERROR "ONNX Runtime not found at ${ORT_DIR}. Download onnxruntime-win-x64 and extract it there.")
endif()

target_include_directories(DLSSCamDemo PRIVATE "${DLSS_SDK}/include" "${ORT_DIR}/include")
target_link_directories(DLSSCamDemo PRIVATE "${DLSS_LIB_DIR}" "${ORT_DIR}/lib")
target_link_libraries(DLSSCamDemo PRIVATE
    d3d12 dxgi dxguid d3dcompiler
    mf mfplat mfreadwrite mfuuid
    windowscodecs ole32 shell32 shlwapi comdlg32 comctl32 user32 gdi32 winmm dwmapi
    nvsdk_ngx_d onnxruntime
)

add_custom_command(TARGET DLSSCamDemo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_DIR}/lib/onnxruntime.dll"
        "$<TARGET_FILE_DIR:DLSSCamDemo>/onnxruntime.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${DLSS_RUNTIME_DEV},${DLSS_RUNTIME_REL}>"
        "$<TARGET_FILE_DIR:DLSSCamDemo>/nvngx_dlss.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/README.md"
        "$<TARGET_FILE_DIR:DLSSCamDemo>/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:DLSSCamDemo>/LICENSE"
)

# Experimental DLSS 5 / Neural Rendering runtime.
# These binaries are never committed. The user stages their own copies in runtime/
# and they are placed beside the executable so ReShade/RenoDX can pick them up.
set(NR_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/runtime")
if(EXISTS "${NR_RUNTIME_DIR}")
    file(GLOB NR_RUNTIME_FILES "${NR_RUNTIME_DIR}/*.dll" "${NR_RUNTIME_DIR}/*.addon64")
    foreach(nrfile ${NR_RUNTIME_FILES})
        get_filename_component(nrname "${nrfile}" NAME)
        add_custom_command(TARGET DLSSCamDemo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${nrfile}" "$<TARGET_FILE_DIR:DLSSCamDemo>/${nrname}")
    endforeach()

    # ReShade configuration carries the RenoDX slider values the user sets in the
    # overlay, so it is staged once and then left alone.
    # *.cfg is Deep Fried Chicken's layer configuration; like ReShade.ini it holds
    # settings the user edits, so it is staged once and never overwritten.
    file(GLOB NR_RUNTIME_INI "${NR_RUNTIME_DIR}/*.ini" "${NR_RUNTIME_DIR}/*.cfg")
    foreach(inifile ${NR_RUNTIME_INI})
        get_filename_component(ininame "${inifile}" NAME)
        add_custom_command(TARGET DLSSCamDemo POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DSRC=${inifile}
                -DDST=$<TARGET_FILE_DIR:DLSSCamDemo>/${ininame}
                -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_absent.cmake")
    endforeach()
endif()

# Person-segmentation model. Downloaded by the user, never committed.
set(SEG_MODEL "${CMAKE_SOURCE_DIR}/models/rvm_mobilenetv3_fp32.onnx")
if(EXISTS "${SEG_MODEL}")
    add_custom_command(TARGET DLSSCamDemo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SEG_MODEL}" "$<TARGET_FILE_DIR:DLSSCamDemo>/rvm_mobilenetv3_fp32.onnx")
endif()

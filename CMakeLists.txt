cmake_minimum_required(VERSION 3.20)
project(AILimitVR C CXX ASM_MASM)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_CXX_STANDARD 20)
set(MINHOOK_DIR "C:/VRLab/bin/minhook" CACHE PATH "MinHook source")
set(OPENXR_DIR "C:/VRLab/bin/openxr_loader_1.1.65" CACHE PATH "Khronos OpenXR loader")
set(IMGUI_DIR "C:/VRLab/bin/imgui/imgui-1.93.0" CACHE PATH "Dear ImGui")
set(OUT "${CMAKE_SOURCE_DIR}/../build")

# winhttp.dll proxy (chain-loads winhttp_chain.dll, then loads AILimitVR\AILimitVRNative.dll)
add_library(winhttp SHARED proxy/winhttp_proxy.cpp proxy/thunks.asm proxy/winhttp.def)
set_target_properties(winhttp PROPERTIES OUTPUT_NAME winhttp RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUT}")

add_library(minhook STATIC ${MINHOOK_DIR}/src/buffer.c ${MINHOOK_DIR}/src/hook.c ${MINHOOK_DIR}/src/trampoline.c ${MINHOOK_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${MINHOOK_DIR}/include)
add_library(AILimitVRNative SHARED native/AILimitVRNative.cpp xr/AIXR.cpp xr/MenuDraw.cpp
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(AILimitVRNative PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends ${OPENXR_DIR}/include)
target_compile_definitions(AILimitVRNative PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(AILimitVRNative PRIVATE minhook d3d11 dxgi winmm ws2_32)
add_custom_command(TARGET AILimitVRNative POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OPENXR_DIR}/x64/bin/openxr_loader.dll" "${OUT}/openxr_loader.dll")
set_target_properties(AILimitVRNative PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUT}")
# Symbols for crash reports (the shipped DLL is the same code; the .pdb and .map stay in the build folder)
target_compile_options(AILimitVRNative PRIVATE /Zi)
target_link_options(AILimitVRNative PRIVATE /DEBUG /OPT:REF /OPT:ICF /MAP)

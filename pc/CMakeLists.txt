cmake_minimum_required(VERSION 3.20)
project(hadal CXX RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # Static CRT
# Version x.y.z, set by the release build
set(HADAL_VERSION "3.0.0" CACHE STRING "Version")
string(REPLACE "." ";" HADAL_V "${HADAL_VERSION}")
list(GET HADAL_V 0 V1)
list(GET HADAL_V 1 V2)
list(GET HADAL_V 2 V3)
add_compile_definitions(UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
add_compile_definitions(HADAL_V1=${V1} HADAL_V2=${V2} HADAL_V3=${V3} HADAL_VSTR="${HADAL_VERSION}")
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/permissive-> $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
# Keep PDBs for crash reports
add_compile_options($<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/Zi>)
add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)

add_executable(hadal-svc svc.cpp hadal.rc)
target_compile_definitions(hadal-svc PRIVATE HADAL_SVC)
target_link_libraries(hadal-svc ws2_32 iphlpapi wtsapi32 powrprof bcrypt advapi32 pdh dxgi setupapi)

add_executable(hadal-tray WIN32 tray.cpp stream.cpp hadal.rc)
target_link_libraries(hadal-tray gdiplus shlwapi ole32 advapi32 windowsapp d3d11 dxgi mfplat mfuuid dbghelp)

# Fuzz harness
option(HADAL_FUZZ "Build the hadal-fuzz target" OFF)
if(HADAL_FUZZ)
    add_executable(hadal-fuzz fuzz.cpp)
    target_compile_options(hadal-fuzz PRIVATE /fsanitize=address /fsanitize=fuzzer /wd4505)
    target_link_options(hadal-fuzz PRIVATE /INCREMENTAL:NO)
    target_link_libraries(hadal-fuzz ws2_32 iphlpapi wtsapi32 powrprof bcrypt advapi32 pdh dxgi setupapi)
endif()

cmake_minimum_required(VERSION 3.20)
project(FeedbackPlease LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FP_BUILD_GUI "Build the FeedbackPlease GUI (fetches GLFW, Dear ImGui, ImPlot)" ON)
option(FP_WITH_DAQMX "Use NI-DAQmx if it is installed" ON)
option(FP_WITH_CLOSEDLOOP "Build the spike-triggered closed-loop tab (needs the SpikeGLX SDK, Windows)" ON)
set(SGLX_SDK_DIR "${CMAKE_SOURCE_DIR}/../LiveSorter_BF/SDK/API"
    CACHE PATH "SpikeGLX SDK API folder (SglxCppClient.cpp, SglxApi.h, libSglxApi.a, SglxApi.dll)")
set(CLOSEDLOOP_FILTERGEN_DIR "${CMAKE_SOURCE_DIR}/../LiveSorter_BF/FilterGen"
    CACHE PATH "Folder holding calibrate_for_closedloop.py (the default calibration script)")
set(NIDAQMX_ROOT "C:/Program Files (x86)/National Instruments/Shared/ExternalCompilerSupport/C"
    CACHE PATH "NI-DAQmx ANSI C support folder (contains include/ and lib64/)")

if(MSVC)
  add_compile_options(/W3 /permissive- /utf-8)
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# ---- Detection core: pure C++, no hardware or GUI dependencies
add_library(fp_core STATIC
  src/core/Params.cpp
  src/core/WavFile.cpp
  src/core/BandEnvelope.cpp
  src/core/Syllables.cpp
  src/core/FeedbackStateMachine.cpp
  src/core/Detector.cpp
  src/core/Filters.cpp
  src/core/NoiseBurst.cpp
  src/core/Offline.cpp
  src/core/SongDetector.cpp
  src/core/SongRecorder.cpp
  src/core/SyllableCodes.cpp
  src/core/SynthSong.cpp
  src/core/SpectralTemplate.cpp)
target_include_directories(fp_core PUBLIC src)

# ---- I/O backends + real-time engine
add_library(fp_engine STATIC
  src/io/SimBackend.cpp
  src/io/DaqBackend.cpp
  src/app/Box.cpp
  src/app/Engine.cpp
  src/io/DaqCard.cpp)
target_link_libraries(fp_engine PUBLIC fp_core)

if(FP_WITH_DAQMX)
  find_path(NIDAQMX_INCLUDE_DIR NIDAQmx.h HINTS "${NIDAQMX_ROOT}/include")
  find_library(NIDAQMX_LIBRARY NIDAQmx HINTS "${NIDAQMX_ROOT}/lib64/msvc")
  if(NIDAQMX_INCLUDE_DIR AND NIDAQMX_LIBRARY)
    message(STATUS "NI-DAQmx: ${NIDAQMX_LIBRARY}")
    target_include_directories(fp_engine PRIVATE "${NIDAQMX_INCLUDE_DIR}")
    target_link_libraries(fp_engine PUBLIC "${NIDAQMX_LIBRARY}")
    target_compile_definitions(fp_engine PRIVATE FP_HAVE_DAQMX)
  else()
    message(WARNING "NI-DAQmx not found; building with the simulated source only. Set NIDAQMX_ROOT.")
  endif()
endif()

# ---- Command-line tools and tests
add_executable(offline_detect tools/offline_detect.cpp)
target_link_libraries(offline_detect PRIVATE fp_core)

add_executable(make_test_wavs tools/make_test_wavs.cpp)
target_link_libraries(make_test_wavs PRIVATE fp_core)

add_executable(fp_tests tests/test_core.cpp)
target_link_libraries(fp_tests PRIVATE fp_core)
add_executable(fp_engine_test tests/test_engine.cpp)
target_link_libraries(fp_engine_test PRIVATE fp_engine)
if(NIDAQMX_INCLUDE_DIR AND NIDAQMX_LIBRARY AND FP_WITH_DAQMX)
  target_include_directories(fp_engine_test PRIVATE "${NIDAQMX_INCLUDE_DIR}")
  target_compile_definitions(fp_engine_test PRIVATE FP_HAVE_DAQMX)
endif()
if(WIN32)
  target_link_libraries(fp_engine_test PRIVATE winmm)
endif()

enable_testing()
add_test(NAME core COMMAND fp_tests)
add_test(NAME engine COMMAND fp_engine_test)

# ---- GUI
if(FP_BUILD_GUI)
  include(FetchContent)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
  FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.90.9 GIT_SHALLOW TRUE)
  FetchContent_Declare(implot GIT_REPOSITORY https://github.com/epezent/implot.git GIT_TAG v0.16 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glfw imgui implot)
  find_package(OpenGL REQUIRED)

  add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp)
  target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp ${implot_SOURCE_DIR})
  target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

  add_executable(FeedbackPlease src/app/main.cpp src/app/Gui.cpp src/app/Widgets.cpp src/app/TuneView.cpp
    src/app/CrashHandler.cpp)
  target_link_libraries(FeedbackPlease PRIVATE fp_engine imgui_lib)

  if(WIN32)
    target_sources(FeedbackPlease PRIVATE src/app/FeedbackPlease.rc)  # icon
  endif()

  # MSN finder tab: drives tools/msn_finder/msn_server.py (numpy + scipy) as a child process.
  if(WIN32)
    target_sources(FeedbackPlease PRIVATE src/app/MsnFinder.cpp src/app/MsnFinderView.cpp)
    target_compile_definitions(FeedbackPlease PRIVATE FP_MSN
      "FP_MSN_DEFAULT_SCRIPT=\"${CMAKE_SOURCE_DIR}/tools/msn_finder/msn_server.py\"")
  endif()

  # Closed-loop tab: the ClosedLoop spike detector, ported into src/closedloop. Optional so the
  # rest builds on machines without the SpikeGLX SDK.
  if(FP_WITH_CLOSEDLOOP AND WIN32 AND EXISTS "${SGLX_SDK_DIR}/SglxCppClient.cpp" AND EXISTS "${SGLX_SDK_DIR}/libSglxApi.a")
    message(STATUS "Closed loop: SpikeGLX SDK in ${SGLX_SDK_DIR}")
    add_library(fp_closedloop STATIC
      src/closedloop/Calibration.cpp
      src/closedloop/Config.cpp
      src/closedloop/ConvolutionEngine.cpp
      src/closedloop/DecisionThread.cpp
      src/closedloop/FilterBank.cpp
      src/closedloop/ImecFetchThread.cpp
      src/closedloop/NiFetchThread.cpp
      src/closedloop/SglxMetaReader.cpp
      "${SGLX_SDK_DIR}/SglxCppClient.cpp")
    target_include_directories(fp_closedloop PUBLIC src "${SGLX_SDK_DIR}")
    # SglxApi.dll is the SDK's import library target; it must sit next to the exe at run time.
    target_link_libraries(fp_closedloop PUBLIC "${SGLX_SDK_DIR}/libSglxApi.a")
    target_sources(FeedbackPlease PRIVATE src/app/ClosedLoop.cpp src/app/ClosedLoopView.cpp)
    target_link_libraries(FeedbackPlease PRIVATE fp_closedloop)
    target_compile_definitions(FeedbackPlease PRIVATE FP_CLOSEDLOOP)
    add_executable(fp_closedloop_test tests/test_closedloop.cpp src/app/ClosedLoop.cpp)
    target_link_libraries(fp_closedloop_test PRIVATE fp_closedloop)
    add_custom_command(TARGET fp_closedloop_test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SGLX_SDK_DIR}/SglxApi.dll" "$<TARGET_FILE_DIR:fp_closedloop_test>")
    add_test(NAME closedloop COMMAND fp_closedloop_test)
    get_filename_component(_cl_script "${CLOSEDLOOP_FILTERGEN_DIR}/calibrate_for_closedloop.py" ABSOLUTE)
    if(EXISTS "${_cl_script}")
      target_compile_definitions(FeedbackPlease PRIVATE "FP_CLOSEDLOOP_DEFAULT_SCRIPT=\"${_cl_script}\"")
    endif()
    add_custom_command(TARGET FeedbackPlease POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SGLX_SDK_DIR}/SglxApi.dll" "$<TARGET_FILE_DIR:FeedbackPlease>")
  elseif(FP_WITH_CLOSEDLOOP)
    message(WARNING "Closed loop tab disabled: SpikeGLX SDK not found in ${SGLX_SDK_DIR}. Set SGLX_SDK_DIR.")
  endif()
  if(WIN32)
    target_link_libraries(FeedbackPlease PRIVATE winmm comdlg32 dbghelp shell32 ole32 psapi)
  endif()
  if(MSVC)
    # Keep symbols in Release so crash traces have function names and line numbers.
    target_compile_options(FeedbackPlease PRIVATE /Zi)
    target_link_options(FeedbackPlease PRIVATE /DEBUG /OPT:REF /OPT:ICF)
  endif()
endif()

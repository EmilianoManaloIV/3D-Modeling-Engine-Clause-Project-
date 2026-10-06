cmake_minimum_required(VERSION 3.16)
project(Modeler3D VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MODELER_BUILD_TESTS "Build the unit tests (modeler_tests)" OFF)

# Geometry / file code with no OpenGL or OS dependency (shared with the tests).
set(CORE_SOURCES
    src/mesh.cpp
    src/uv.cpp
    src/parametric.cpp
    src/scene.cpp
    src/skin.cpp
    src/particles.cpp
    src/transform.cpp
    src/profiler.cpp
    src/jobs.cpp
    src/polygon.cpp
    src/csg.cpp
    src/expr.cpp
    src/meshedit.cpp
    src/meshtools.cpp
    src/image_load.cpp
    src/input_map.cpp
    src/bvh.cpp
    src/pathtracer.cpp)

set(APP_SOURCES
    src/main.cpp
    src/editor.cpp
    src/editor_actions.cpp
    src/editor_ui.cpp
    src/editor_props.cpp
    src/editor_commands.cpp
    src/editor_render.cpp
    src/editor_gizmo.cpp
    src/editor_bench.cpp
    src/editor_stress.cpp
    src/editor_meshops.cpp
    src/editor_editmode.cpp
    src/editor_tools.cpp
    src/editor_renderview.cpp
    src/gpu_tracer.cpp
    src/hwrt.cpp
    src/renderer.cpp
    src/ui.cpp
    src/font.cpp
    src/gl.cpp
    src/image_io.cpp)

if(WIN32)
    list(APPEND APP_SOURCES src/platform_win32.cpp)
else()
    list(APPEND APP_SOURCES src/platform_x11.cpp)
endif()

# WIN32 = GUI program on Windows (no console window); ignored elsewhere.
add_executable(Modeler3D WIN32 ${APP_SOURCES} ${CORE_SOURCES})
target_include_directories(Modeler3D PRIVATE src)

# The job system (src/jobs.cpp) uses std::thread.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(Modeler3D PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(Modeler3D PRIVATE /W3)
else()
    target_compile_options(Modeler3D PRIVATE -Wall -Wextra -Wno-cast-function-type)
endif()

if(WIN32)
    target_link_libraries(Modeler3D PRIVATE opengl32 gdi32 user32 shell32 comdlg32 psapi)
    if(MINGW)
        # Fully static: the .exe runs without any MinGW DLLs next to it.
        target_link_options(Modeler3D PRIVATE -static -static-libgcc -static-libstdc++)
    elseif(MSVC)
        target_link_options(Modeler3D PRIVATE /ENTRY:mainCRTStartup)
        set_property(TARGET Modeler3D PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
else()
    # Link plain libGL (contains GLX on every distro) rather than GLVND's split libs.
    set(OpenGL_GL_PREFERENCE LEGACY)
    find_package(OpenGL REQUIRED)
    find_package(X11 REQUIRED)
    target_include_directories(Modeler3D PRIVATE ${X11_INCLUDE_DIR} ${OPENGL_INCLUDE_DIR})
    target_link_libraries(Modeler3D PRIVATE ${OPENGL_gl_LIBRARY} ${X11_LIBRARIES})
    # Bundle the C++ runtime so the binary runs on other distros too.
    target_link_options(Modeler3D PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(MODELER_BUILD_TESTS)
    add_executable(modeler_tests tests/tests.cpp src/image_io.cpp src/hwrt.cpp ${CORE_SOURCES})
    target_compile_definitions(modeler_tests PRIVATE MODELER_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    target_include_directories(modeler_tests PRIVATE src)
    target_link_libraries(modeler_tests PRIVATE Threads::Threads)
    if(MINGW)
        target_link_options(modeler_tests PRIVATE -static)
    endif()
endif()

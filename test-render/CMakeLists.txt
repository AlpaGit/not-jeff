cmake_minimum_required(VERSION 3.16)
project(TestSwfRendering C)
set(CMAKE_C_STANDARD 11)

# Optionnel : Release par défaut en local
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)

# ---- raylib (dep OpenGL/winmm/gdi32 sous MinGW) ----
FetchContent_Declare(raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 5.0  # ou latest; 5.0 fonctionne bien sous MinGW
)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(raylib)

# ---- physfs ----
FetchContent_Declare(physfs
        GIT_REPOSITORY https://github.com/icculus/physfs.git
        GIT_TAG release-3.2.0  # une version stable
)
# on veut une lib statique simple
set(PHYSFS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PHYSFS_BUILD_TEST   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(physfs)

# Ton exécutable
add_executable(TestSwfRendering
        main.c
        cJSON.c
)

target_include_directories(TestSwfRendering PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}             # pour cJSON.h, raygui.h si tu l'as à côté
)

# Liaisons
target_link_libraries(TestSwfRendering PRIVATE raylib physfs-static)

# Sous Windows/MinGW, ajoute aussi les libs système nécessaires
if (WIN32)
    target_link_libraries(TestSwfRendering PRIVATE winmm gdi32 opengl32)
endif()

target_compile_definitions(TestSwfRendering PRIVATE SUPPORT_FILEFORMAT_DDS)

# Option : dossier working dir de CLion = répertoire du binaire
set_property(TARGET TestSwfRendering PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

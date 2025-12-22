cmake_minimum_required(VERSION 3.15)
project(WebcamLib VERSION 1.0.0 LANGUAGES C CXX)

# Opciones de compilacion
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Directorios de salida (bin y lib juntos)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Flags para Windows
if(WIN32)
    add_definitions(-DBUILDING_DLL)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-DUNICODE -D_UNICODE)
endif()

# Fuentes
set(LIB_SOURCES src/webcam_common.c)

if(WIN32)
    list(APPEND LIB_SOURCES src/webcam_win.cpp)
    # Librerias nativas de Windows necesarias
    set(PLATFORM_LIBS mf mfplat mfreadwrite mfuuid ole32 user32 shlwapi) 
elseif(UNIX)
    list(APPEND LIB_SOURCES src/webcam_linux.c)
    set(PLATFORM_LIBS )
endif()

# Crear DLL / Shared Lib
add_library(webcam SHARED ${LIB_SOURCES})
target_include_directories(webcam PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(webcam PRIVATE ${PLATFORM_LIBS})

# Ejecutable de Ejemplo
add_executable(demo_app examples/main.c)
target_link_libraries(demo_app webcam)

# Copiar la DLL al lado del .exe en Windows (Post-Build Event)
if(WIN32)
    add_custom_command(TARGET demo_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:webcam>
        $<TARGET_FILE_DIR:demo_app>
        COMMENT "Copiando DLL al directorio del ejecutable..."
    )
endif()
cmake_minimum_required(VERSION 3.16)
project(PhotonNoiseDemo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Find OpenSceneGraph ─────────────────────────────────────────────────
find_package(OpenSceneGraph 3.6 REQUIRED COMPONENTS
    osg osgDB osgGA osgViewer osgUtil OpenThreads)

find_package(OpenGL REQUIRED)

# ── Executable ──────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/PostProcessing.cpp
    src/PostProcessChain.cpp
)

set(HEADERS
    src/INoiseEffect.h
    src/PostProcessing.h
    src/PostProcessChain.h
    src/PhotonNoiseEffect.h
    src/DarkNoiseEffect.h
    src/ReadNoiseEffect.h
    src/PRNUEffect.h
    src/SensorNoiseSimulator.h
)

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${OPENSCENEGRAPH_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OPENSCENEGRAPH_LIBRARIES}
    ${OPENGL_LIBRARIES}
)

# ── Copy shaders to build directory ─────────────────────────────────────
file(GLOB SHADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${SHADER_FILES}
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders/"
    COMMENT "Copying shaders to output directory"
)

# ── Install ─────────────────────────────────────────────────────────────
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
install(DIRECTORY shaders/ DESTINATION bin/shaders)

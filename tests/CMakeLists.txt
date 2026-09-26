cmake_minimum_required(VERSION 3.10...3.31)

set(CMAKE_AUTOMOC ON)

find_package(Qt6 ${REQUIRED_QT_VERSION} REQUIRED COMPONENTS Test)

set(TEST_INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/core
  ${CMAKE_SOURCE_DIR}/src/shared
  ${CMAKE_SOURCE_DIR}/external
  ${CMAKE_SOURCE_DIR}/src/player
)

set(TEST_COMPILE_DEFS
  PREFIX="${CMAKE_INSTALL_PREFIX}"
)

add_executable(test_systemcomponent test_systemcomponent.cpp)
target_link_libraries(test_systemcomponent jmp_core Qt6::Test)
target_include_directories(test_systemcomponent PRIVATE ${TEST_INCLUDE_DIRS})
target_compile_definitions(test_systemcomponent PRIVATE ${TEST_COMPILE_DEFS})
add_test(NAME test_systemcomponent COMMAND test_systemcomponent)

add_executable(test_log test_log.cpp)
target_link_libraries(test_log jmp_core Qt6::Test)
target_include_directories(test_log PRIVATE ${TEST_INCLUDE_DIRS})
target_compile_definitions(test_log PRIVATE ${TEST_COMPILE_DEFS})
add_test(NAME test_log COMMAND test_log)

add_executable(test_settings test_settings.cpp)
target_link_libraries(test_settings jmp_core Qt6::Test)
target_include_directories(test_settings PRIVATE ${TEST_INCLUDE_DIRS})
target_compile_definitions(test_settings PRIVATE ${TEST_COMPILE_DEFS})
add_test(NAME test_settings COMMAND test_settings)

add_executable(test_displaymanager test_displaymanager.cpp)
target_link_libraries(test_displaymanager jmp_core Qt6::Test)
target_include_directories(test_displaymanager PRIVATE ${TEST_INCLUDE_DIRS})
target_compile_definitions(test_displaymanager PRIVATE ${TEST_COMPILE_DEFS})
add_test(NAME test_displaymanager COMMAND test_displaymanager)

# The external controller only needs Qt Core/Network, so it also has a standalone
# test build in tests/external-player for machines without WebEngine or libmpv.
add_executable(test_externalplayercontroller
  test_externalplayercontroller.cpp
  ../src/player/ExternalPlayerController.cpp
  ../src/player/ExternalPlayerController.h
)
target_link_libraries(test_externalplayercontroller Qt6::Core Qt6::Network Qt6::Test)
target_include_directories(test_externalplayercontroller PRIVATE ${CMAKE_SOURCE_DIR}/src/player)
if(WIN32)
  target_link_libraries(test_externalplayercontroller user32)
endif()
if(WIN32)
  # Qt Test may send default output to a debugger for Windows executables.
  # Keep an explicit file so CI can always show the failing assertion.
  add_test(NAME test_externalplayercontroller COMMAND test_externalplayercontroller
    -o ${CMAKE_CURRENT_BINARY_DIR}/external-controller-results.txt,txt)
else()
  add_test(NAME test_externalplayercontroller COMMAND test_externalplayercontroller)
endif()

find_program(NODE_EXECUTABLE NAMES node nodejs)
if(NODE_EXECUTABLE)
  add_test(NAME test_externalvideoplayer
    COMMAND ${NODE_EXECUTABLE} --test ${CMAKE_CURRENT_SOURCE_DIR}/test_externalvideoplayer.js)
endif()

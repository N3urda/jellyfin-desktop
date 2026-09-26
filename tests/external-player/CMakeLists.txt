cmake_minimum_required(VERSION 3.16)
project(ExternalPlayerControllerTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Network Test)
add_executable(test_externalplayercontroller
  ../test_externalplayercontroller.cpp
  ../../src/player/ExternalPlayerController.cpp
  ../../src/player/ExternalPlayerController.h)
target_include_directories(test_externalplayercontroller PRIVATE ../../src/player)
target_link_libraries(test_externalplayercontroller PRIVATE Qt6::Core Qt6::Network Qt6::Test)
if(WIN32)
  target_link_libraries(test_externalplayercontroller PRIVATE user32)
endif()
enable_testing()
add_test(NAME externalplayercontroller COMMAND test_externalplayercontroller)

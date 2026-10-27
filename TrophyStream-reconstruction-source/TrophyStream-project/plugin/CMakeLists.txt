cmake_minimum_required(VERSION 3.28)
project(trophy-stream VERSION 0.2.0 LANGUAGES C)

if(NOT WIN32)
  message(FATAL_ERROR "TrophyStream natif cible Windows uniquement pour cette version.")
endif()

find_package(libobs CONFIG REQUIRED)

add_library(trophy-stream MODULE trophy-stream.c)
target_compile_features(trophy-stream PRIVATE c_std_11)
target_compile_definitions(trophy-stream PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if(TARGET OBS::libobs)
  target_link_libraries(trophy-stream PRIVATE OBS::libobs)
elseif(TARGET libobs)
  target_link_libraries(trophy-stream PRIVATE libobs)
else()
  message(FATAL_ERROR "Cible libobs introuvable dans les dépendances OBS.")
endif()

target_link_libraries(trophy-stream PRIVATE ws2_32 urlmon windowscodecs winmm msimg32 bcrypt ole32)
set_target_properties(trophy-stream PROPERTIES PREFIX "" OUTPUT_NAME "trophy-stream")

install(TARGETS trophy-stream RUNTIME DESTINATION "trophy-stream/bin/64bit" LIBRARY DESTINATION "trophy-stream/bin/64bit")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../data/" DESTINATION "trophy-stream/data")

cmake_minimum_required(VERSION 3.10)
project(GameMusicPlayer CXX)

option(GME_UNRAR "Enable RAR file format (optional, requires UnRAR library)" ON)

find_package(SDL2 REQUIRED)
find_package(SDL2_ttf REQUIRED)

if(GME_UNRAR)
  find_package(UNRAR QUIET)
endif()

set(player_SRCS
    Audio_Scope.cpp
    Music_Player.cpp
    Archive_Reader.cpp
    player.cpp
)

add_executable(gme_player ${player_SRCS})

target_include_directories(gme_player PRIVATE
    ${PROJECT_SOURCE_DIR}/gme
)

set_property(TARGET gme_player PROPERTY CXX_STANDARD 11)
set_property(TARGET gme_player PROPERTY CXX_STANDARD_REQUIRED ON)


target_link_libraries(gme_player PRIVATE
    SDL2::SDL2
    SDL2_ttf::SDL2_ttf
    gme::gme
)

if(GME_UNRAR)
  if(UNRAR_FOUND)
    message(STATUS "UnRAR library located, enabling RAR format support")
    target_compile_definitions(gme_player PRIVATE RARDLL)
    if(RAR_HDR_UNRAR_H)
      target_compile_definitions(gme_player PRIVATE RAR_HDR_UNRAR_H)
    elseif(RAR_HDR_DLL_HPP)
      target_compile_definitions(gme_player PRIVATE RAR_HDR_DLL_HPP)
    endif()
    target_include_directories(gme_player PRIVATE ${UNRAR_INCLUDE_DIRS})
    target_link_libraries(gme_player PRIVATE ${UNRAR_LIBRARIES})
    target_compile_options(gme_player PRIVATE -pthread)
	target_link_options(gme_player PRIVATE -pthread)
  else()
    message(STATUS "UnRAR library not found, RAR support disabled")
  endif()
else()
  message(STATUS "RAR file format support excluded by configuration")
endif()

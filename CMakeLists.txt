cmake_minimum_required(VERSION 3.25)
project(vn-discord-rpc VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(third_party/discord-presence EXCLUDE_FROM_ALL)
add_subdirectory(third_party/nlohmann_json EXCLUDE_FROM_ALL)

# ── System packages ──
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)

if (NOT CURL_FOUND)
	message(FATAL_ERROR "libcurl not found. Install: sudo apt install libcurl4-openssl-dev")
endif()

# ── Sources ──
set(SOURCES
	src/main.cpp
	src/process_scanner.cpp
	src/vndb_client.cpp
	src/vn_cache.cpp
	src/ignore_list.cpp
	src/lutris_db.cpp
	src/rpc_manager.cpp
	src/steam_detector.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${CURL_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
	discord-rpc
	nlohmann_json::nlohmann_json
	${CURL_LIBRARIES}
	SQLite::SQLite3
	pthread
)

target_compile_options(${PROJECT_NAME} PRIVATE
	-Wall -Wextra -Wpedantic
	$<$<CONFIG:Release>:-O2>
	$<$<CONFIG:Debug>:-Og -g>
)

# ── Install ──
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
install(FILES
	${CMAKE_CURRENT_SOURCE_DIR}/data/vn-discord-rpc.service
	DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user
	OPTIONAL
)

message(STATUS "──────────────────────────────────────────")
message(STATUS " vn-discord-rpc  v${PROJECT_VERSION}")
message(STATUS " Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS " CURL libs:      ${CURL_LIBRARIES}")
message(STATUS " SQLite3:        ${SQLite3_VERSION}")
message(STATUS "──────────────────────────────────────────")

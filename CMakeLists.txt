cmake_minimum_required(VERSION 3.30)

project(edgefleet VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Catch2 3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PostgreSQL QUIET)
find_package(unofficial-argon2 CONFIG REQUIRED)
find_package(DuckDB CONFIG QUIET)
find_package(Drogon CONFIG QUIET)
find_package(spdlog CONFIG QUIET)

enable_testing()
include(CTest)
include(Catch)

set(EDGEFLEET_CORE_SOURCES
  src/shared/types.cpp
  src/shared/canonical_json.cpp
  src/shared/digest_service.cpp
  src/shared/config.cpp
  src/shared/secret_resolver.cpp
  src/shared/job_lease_store.cpp
  src/shared/http_client_pool.cpp
  src/shared/logger.cpp
  src/shared/tenant_clock.cpp
  src/domain/safety.cpp
  src/domain/artifact.cpp
  src/domain/benchmark.cpp
  src/domain/replay.cpp
  src/infrastructure/sqlite_storage.cpp
  src/infrastructure/integrations.cpp
  src/application/control_plane.cpp
  src/application/interoperability.cpp
  src/application/jobs.cpp
  src/application/evidence_export.cpp
  src/web/http_server.cpp
  src/web/policy_registry.cpp
)
add_library(edgefleet_core STATIC ${EDGEFLEET_CORE_SOURCES})
target_include_directories(edgefleet_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(edgefleet_core PUBLIC nlohmann_json::nlohmann_json unofficial::sqlite3::sqlite3 OpenSSL::Crypto unofficial::argon2::libargon2)
if(PostgreSQL_FOUND)
  target_sources(edgefleet_core PRIVATE src/infrastructure/postgres_storage.cpp)
  target_link_libraries(edgefleet_core PUBLIC PostgreSQL::PostgreSQL)
  target_compile_definitions(edgefleet_core PUBLIC EDGEFLEET_HAS_POSTGRES=1)
endif()
if(TARGET Drogon::Drogon)
  target_link_libraries(edgefleet_core PUBLIC Drogon::Drogon)
  target_compile_definitions(edgefleet_core PUBLIC EDGEFLEET_HAS_DROGON=1)
endif()
if(TARGET spdlog::spdlog)
  target_link_libraries(edgefleet_core PUBLIC spdlog::spdlog)
  target_compile_definitions(edgefleet_core PUBLIC EDGEFLEET_HAS_SPDLOG=1)
elseif(TARGET spdlog::spdlog_header_only)
  target_link_libraries(edgefleet_core PUBLIC spdlog::spdlog_header_only)
  target_compile_definitions(edgefleet_core PUBLIC EDGEFLEET_HAS_SPDLOG=1)
endif()
if(TARGET duckdb)
  target_link_libraries(edgefleet_core PUBLIC duckdb)
  target_compile_definitions(edgefleet_core PUBLIC EDGEFLEET_HAS_DUCKDB=1)
elseif(TARGET duckdb_static)
  target_link_libraries(edgefleet_core PUBLIC duckdb_static)
  target_compile_definitions(edgefleet_core PUBLIC EDGEFLEET_HAS_DUCKDB=1)
endif()
target_compile_options(edgefleet_core PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)

add_executable(edgefleet src/cli/main.cpp)
target_link_libraries(edgefleet PRIVATE edgefleet_core)
target_compile_options(edgefleet PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)
if(WIN32)
  target_link_libraries(edgefleet_core PUBLIC ws2_32 advapi32)
endif()

add_executable(edgefleet_tests
  tests/unit/foundation_test.cpp
  tests/unit/safety_test.cpp
  tests/unit/artifact_test.cpp
  tests/unit/benchmark_test.cpp
  tests/component/storage_test.cpp
  tests/component/jobs_test.cpp
  tests/component/runtime_contract_test.cpp
  tests/component/recovery_test.cpp
  tests/component/performance_test.cpp
  tests/component/policy_test.cpp
  tests/component/interoperability_test.cpp
  tests/contract/integrations/iot_rest_v1_contract_test.cpp
  tests/contract/integrations/notification_hub_v1_contract_test.cpp
  tests/contract/integrations/workflow_manual_v1_contract_test.cpp
  tests/integration/http_test.cpp
  tests/integration/tenant_isolation_test.cpp
  tests/integration/standalone_journey_test.cpp
)
target_compile_definitions(edgefleet_tests PRIVATE EDGEFLEET_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(edgefleet_tests PRIVATE edgefleet_core Catch2::Catch2WithMain)
target_compile_options(edgefleet_tests PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)
catch_discover_tests(edgefleet_tests)

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
  file(GLOB_RECURSE FORMAT_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
  add_custom_target(format COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${FORMAT_FILES} VERBATIM)
else()
  add_custom_target(format COMMAND ${CMAKE_COMMAND} -E echo "clang-format is not installed; install it before running the formatting gate" COMMAND ${CMAKE_COMMAND} -E false VERBATIM)
endif()

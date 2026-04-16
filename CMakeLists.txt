cmake_minimum_required(VERSION 3.22)
project(waxcpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(waxcpp_apply_debug_suffix target_name)
  if (NOT TARGET "${target_name}")
    return()
  endif()
  get_target_property(_type "${target_name}" TYPE)
  if (_type STREQUAL "EXECUTABLE")
    set_property(TARGET "${target_name}" PROPERTY OUTPUT_NAME_DEBUG "${target_name}_d")
  elseif (_type STREQUAL "STATIC_LIBRARY" OR
          _type STREQUAL "SHARED_LIBRARY" OR
          _type STREQUAL "MODULE_LIBRARY")
    set_property(TARGET "${target_name}" PROPERTY DEBUG_POSTFIX "_d")
  endif()
endfunction()

if (MSVC)
  # Avoid parallel compiler writes to shared PDBs when reusing PCH across targets.
  # /MP enables multi-process compilation per target.
  # /FS keeps shared-PDB access safe under parallel cl.exe workers.
  add_compile_options(/FS /MP)
endif()

option(WAXCPP_BUILD_TESTS "Build waxcpp tests" ON)
option(WAXCPP_BUILD_SERVER "Build waxcpp server targets" ON)
option(WAXCPP_REQUIRE_PARITY_FIXTURES "Fail parity test if no .mv2s fixtures are found" OFF)
option(WAXCPP_REQUIRE_SWIFT_FIXTURES "Fail parity test if no non-synthetic fixtures are found" OFF)
option(WAXCPP_ENABLE_SQLITE_BACKEND "Enable SQLite/FTS5 backend when bundled sqlite amalgamation exists" ON)
option(WAXCPP_ENABLE_LIBTORCH_RUNTIME "Enable real LibTorch runtime backend for MiniLMEmbedderTorch (experimental; OFF by default)" OFF)
set(WAXCPP_LIBTORCH_ROOT "" CACHE PATH "Optional path to an unpacked LibTorch distribution root")
set(WAXCPP_TORCH_RUNTIME_DEFINE 0)

set(WAXCPP_PUBLIC_HEADERS
  include/waxcpp/access_stats.hpp
  include/waxcpp/answer_extractor.hpp
  include/waxcpp/embeddings.hpp
  include/waxcpp/fts5_search_engine.hpp
  include/waxcpp/importance_scorer.hpp
  include/waxcpp/memory_orchestrator.hpp
  include/waxcpp/mv2v_format.hpp
  include/waxcpp/pch.hpp
  include/waxcpp/query_analyzer.hpp
  include/waxcpp/runtime_model_config.hpp
  include/waxcpp/search.hpp
  include/waxcpp/maintenance.hpp
  include/waxcpp/surrogate_generator.hpp
  include/waxcpp/live_set_rewrite.hpp
  include/waxcpp/embedding_memoizer.hpp
  include/waxcpp/text_chunker.hpp
  include/waxcpp/tier_selector.hpp
  include/waxcpp/token_counter.hpp
  include/waxcpp/structured_memory.hpp
  include/waxcpp/types.hpp
  include/waxcpp/vector_math.hpp
  include/waxcpp/vector_engine.hpp
  include/waxcpp/wax_store.hpp
)

add_library(waxcpp STATIC
  src/core/mv2s_format.cpp
  src/core/sha256.cpp
  src/core/wal_ring.cpp
  src/core/wax_store.cpp
  src/text/fts5_search_engine.cpp
  src/text/structured_memory_store.cpp
  src/vector/usearch_vector_engine.cpp
  src/vector/mv2v_format.cpp
  src/rag/access_stats.cpp
  src/rag/answer_extractor.cpp
  src/rag/embedding_memoizer.cpp
  src/rag/importance_scorer.cpp
  src/rag/maintenance.cpp
  src/rag/query_analyzer.cpp
  src/rag/runtime_model_config.cpp
  src/rag/search.cpp
  src/rag/surrogate_generator.cpp
  src/rag/text_chunker.cpp
  src/rag/tier_selector.cpp
  src/rag/token_counter.cpp
  src/rag/vector_math.cpp
  src/rag/live_set_rewrite.cpp
  src/rag/embeddings.cpp
  src/orchestrator/memory_orchestrator.cpp
)

# --- Locate SQLite amalgamation ---
# Primary: third_party/sqlite/ (if someone drops the amalgamation there)
# Fallback: Poco's bundled amalgamation at third_party/poco/dependencies/sqlite3/src/
set(WAXCPP_SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
set(WAXCPP_SQLITE_C "${WAXCPP_SQLITE_DIR}/sqlite3.c")
set(WAXCPP_SQLITE_H "${WAXCPP_SQLITE_DIR}/sqlite3.h")

if (NOT EXISTS "${WAXCPP_SQLITE_C}" OR NOT EXISTS "${WAXCPP_SQLITE_H}")
  set(WAXCPP_SQLITE_POCO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/poco/dependencies/sqlite3/src")
  if (EXISTS "${WAXCPP_SQLITE_POCO_DIR}/sqlite3.c" AND EXISTS "${WAXCPP_SQLITE_POCO_DIR}/sqlite3.h")
    set(WAXCPP_SQLITE_DIR "${WAXCPP_SQLITE_POCO_DIR}")
    set(WAXCPP_SQLITE_C "${WAXCPP_SQLITE_DIR}/sqlite3.c")
    set(WAXCPP_SQLITE_H "${WAXCPP_SQLITE_DIR}/sqlite3.h")
  endif()
endif()

if (WAXCPP_ENABLE_SQLITE_BACKEND AND EXISTS "${WAXCPP_SQLITE_C}" AND EXISTS "${WAXCPP_SQLITE_H}")
  message(STATUS "waxcpp: SQLite/FTS5 amalgamation found at ${WAXCPP_SQLITE_DIR}")
  add_library(waxcpp_sqlite3 STATIC "${WAXCPP_SQLITE_C}")
  target_include_directories(waxcpp_sqlite3 PUBLIC "${WAXCPP_SQLITE_DIR}")
  target_compile_definitions(waxcpp_sqlite3
    PUBLIC
      SQLITE_THREADSAFE=1
      SQLITE_ENABLE_FTS5
      SQLITE_OMIT_LOAD_EXTENSION
  )
elseif (WAXCPP_ENABLE_SQLITE_BACKEND)
  message(WARNING "waxcpp: WAXCPP_ENABLE_SQLITE_BACKEND=ON but sqlite3.c not found; FTS5 disabled")
endif()

target_sources(waxcpp PRIVATE ${WAXCPP_PUBLIC_HEADERS})
source_group(
  TREE ${CMAKE_CURRENT_SOURCE_DIR}/include
  PREFIX "Header Files"
  FILES ${WAXCPP_PUBLIC_HEADERS}
)

target_include_directories(waxcpp
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (TARGET waxcpp_sqlite3)
  target_link_libraries(waxcpp PRIVATE waxcpp_sqlite3)
  target_compile_definitions(waxcpp PRIVATE WAXCPP_HAS_SQLITE=1)
endif()

if (WAXCPP_ENABLE_LIBTORCH_RUNTIME)
  if (NOT WAXCPP_LIBTORCH_ROOT AND (NOT DEFINED Torch_DIR OR Torch_DIR STREQUAL ""))
    message(FATAL_ERROR
      "WAXCPP_ENABLE_LIBTORCH_RUNTIME=ON requires explicit LibTorch location.\n"
      "Set one of:\n"
      "  -DWAXCPP_LIBTORCH_ROOT=/abs/path/to/libtorch\n"
      "  -DTorch_DIR=/abs/path/to/libtorch/share/cmake/Torch\n"
      "Automatic system Torch auto-discovery is disabled for deterministic builds.")
  endif()

  if (DEFINED Torch_DIR AND NOT Torch_DIR STREQUAL "")
    file(TO_CMAKE_PATH "${Torch_DIR}" Torch_DIR)
    if (NOT EXISTS "${Torch_DIR}/TorchConfig.cmake")
      message(FATAL_ERROR
        "Torch_DIR is set but TorchConfig.cmake was not found:\n"
        "  ${Torch_DIR}/TorchConfig.cmake")
    endif()
  endif()

  if (WAXCPP_LIBTORCH_ROOT)
    file(TO_CMAKE_PATH "${WAXCPP_LIBTORCH_ROOT}" WAXCPP_LIBTORCH_ROOT_NORMALIZED)
    set(WAXCPP_LIBTORCH_TORCH_DIR_A "${WAXCPP_LIBTORCH_ROOT_NORMALIZED}/share/cmake/Torch")
    set(WAXCPP_LIBTORCH_TORCH_DIR_B "${WAXCPP_LIBTORCH_ROOT_NORMALIZED}/libtorch/share/cmake/Torch")

    if (DEFINED Torch_DIR AND NOT Torch_DIR STREQUAL "")
      message(STATUS "WAXCPP_LIBTORCH_ROOT is set; using explicit Torch_DIR='${Torch_DIR}'")
    elseif (EXISTS "${WAXCPP_LIBTORCH_TORCH_DIR_A}/TorchConfig.cmake")
      set(Torch_DIR "${WAXCPP_LIBTORCH_TORCH_DIR_A}")
      list(PREPEND CMAKE_PREFIX_PATH "${WAXCPP_LIBTORCH_ROOT_NORMALIZED}")
      message(STATUS "Resolved Torch_DIR from WAXCPP_LIBTORCH_ROOT: ${Torch_DIR}")
    elseif (EXISTS "${WAXCPP_LIBTORCH_TORCH_DIR_B}/TorchConfig.cmake")
      set(Torch_DIR "${WAXCPP_LIBTORCH_TORCH_DIR_B}")
      list(PREPEND CMAKE_PREFIX_PATH "${WAXCPP_LIBTORCH_ROOT_NORMALIZED}/libtorch")
      message(STATUS "Resolved Torch_DIR from nested WAXCPP_LIBTORCH_ROOT/libtorch: ${Torch_DIR}")
    else()
      message(FATAL_ERROR
        "WAXCPP_ENABLE_LIBTORCH_RUNTIME=ON but WAXCPP_LIBTORCH_ROOT does not contain TorchConfig.cmake.\n"
        "Expected one of:\n"
        "  ${WAXCPP_LIBTORCH_TORCH_DIR_A}/TorchConfig.cmake\n"
        "  ${WAXCPP_LIBTORCH_TORCH_DIR_B}/TorchConfig.cmake")
    endif()
  endif()

  find_package(Torch REQUIRED CONFIG PATHS "${Torch_DIR}" NO_DEFAULT_PATH)
  target_link_libraries(waxcpp PRIVATE ${TORCH_LIBRARIES})
  if (DEFINED TORCH_INCLUDE_DIRS)
    target_include_directories(waxcpp PRIVATE ${TORCH_INCLUDE_DIRS})
  endif()
  if (DEFINED TORCH_CXX_FLAGS)
    separate_arguments(WAXCPP_TORCH_CXX_FLAGS NATIVE_COMMAND "${TORCH_CXX_FLAGS}")
    target_compile_options(waxcpp PRIVATE ${WAXCPP_TORCH_CXX_FLAGS})
  endif()
  set(WAXCPP_TORCH_RUNTIME_DEFINE 1)
endif()

target_compile_definitions(waxcpp PUBLIC WAXCPP_HAS_TORCH_RUNTIME=${WAXCPP_TORCH_RUNTIME_DEFINE})

target_compile_features(waxcpp PUBLIC cxx_std_20)
target_precompile_headers(waxcpp
  PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/waxcpp/pch.hpp>"
)

if (MSVC)
  target_compile_options(waxcpp PRIVATE /W4 /permissive-)
else()
  target_compile_options(waxcpp PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (WAXCPP_BUILD_SERVER)
  add_subdirectory(server)

  add_custom_target(run_rag_server
    COMMAND ${CMAKE_COMMAND} -E env
      WAXCPP_TEST_LOG=$<IF:$<BOOL:$ENV{WAXCPP_TEST_LOG}>,1,0>
      WAXCPP_LIBTORCH_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/manifest/libtorch-manifest.json
      $<TARGET_FILE:waxcpp_rag_server>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS waxcpp_rag_server
    COMMENT "Start WAX RAG server on port 8080"
    VERBATIM
  )
endif()

if (WAXCPP_BUILD_TESTS)
  enable_testing()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if (Python3_Interpreter_FOUND)
    add_test(
      NAME waxcpp_verify_submodules_policy_test
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_submodules.py
    )
  endif()

  add_executable(waxcpp_smoke_test tests/unit/smoke_test.cpp)
  target_link_libraries(waxcpp_smoke_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_smoke_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_smoke_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_smoke_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_smoke_test COMMAND waxcpp_smoke_test)

  add_executable(waxcpp_wax_store_verify_test tests/unit/wax_store_verify_test.cpp)
  target_link_libraries(waxcpp_wax_store_verify_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_wax_store_verify_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_wax_store_verify_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_wax_store_verify_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_wax_store_verify_test COMMAND waxcpp_wax_store_verify_test)

  add_executable(waxcpp_wax_store_write_test tests/unit/wax_store_write_test.cpp)
  target_link_libraries(waxcpp_wax_store_write_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_wax_store_write_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_wax_store_write_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_wax_store_write_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_wax_store_write_test COMMAND waxcpp_wax_store_write_test)

  add_executable(waxcpp_wax_store_bulk_ingest_test tests/unit/wax_store_bulk_ingest_test.cpp)
  target_link_libraries(waxcpp_wax_store_bulk_ingest_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_wax_store_bulk_ingest_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_wax_store_bulk_ingest_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_wax_store_bulk_ingest_test REUSE_FROM waxcpp)

  add_executable(waxcpp_mv2s_format_test tests/unit/mv2s_format_test.cpp)
  target_link_libraries(waxcpp_mv2s_format_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_mv2s_format_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_mv2s_format_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_mv2s_format_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_mv2s_format_test COMMAND waxcpp_mv2s_format_test)

  add_executable(waxcpp_fts5_search_engine_test tests/unit/fts5_search_engine_test.cpp)
  target_link_libraries(waxcpp_fts5_search_engine_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_fts5_search_engine_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_fts5_search_engine_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_fts5_search_engine_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_fts5_search_engine_test COMMAND waxcpp_fts5_search_engine_test)

  add_executable(waxcpp_usearch_vector_engine_test tests/unit/usearch_vector_engine_test.cpp)
  target_link_libraries(waxcpp_usearch_vector_engine_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_usearch_vector_engine_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_usearch_vector_engine_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_usearch_vector_engine_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_usearch_vector_engine_test COMMAND waxcpp_usearch_vector_engine_test)

  add_executable(waxcpp_mv2v_format_test tests/unit/mv2v_format_test.cpp)
  target_link_libraries(waxcpp_mv2v_format_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_mv2v_format_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_mv2v_format_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_mv2v_format_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_mv2v_format_test COMMAND waxcpp_mv2v_format_test)

  add_executable(waxcpp_embeddings_test tests/unit/embeddings_test.cpp)
  target_link_libraries(waxcpp_embeddings_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_embeddings_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_embeddings_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_embeddings_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_embeddings_test COMMAND waxcpp_embeddings_test)

  add_executable(waxcpp_libtorch_manifest_gate_test tests/unit/libtorch_manifest_gate_test.cpp)
  target_link_libraries(waxcpp_libtorch_manifest_gate_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_libtorch_manifest_gate_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_libtorch_manifest_gate_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_libtorch_manifest_gate_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_libtorch_manifest_gate_test COMMAND waxcpp_libtorch_manifest_gate_test)

  add_executable(waxcpp_memory_orchestrator_test tests/unit/memory_orchestrator_test.cpp)
  target_link_libraries(waxcpp_memory_orchestrator_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_memory_orchestrator_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_memory_orchestrator_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_memory_orchestrator_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_memory_orchestrator_test COMMAND waxcpp_memory_orchestrator_test)

  add_executable(waxcpp_search_test tests/unit/search_test.cpp)
  target_link_libraries(waxcpp_search_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_search_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_search_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_search_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_search_test COMMAND waxcpp_search_test)

  add_executable(waxcpp_runtime_model_config_test tests/unit/runtime_model_config_test.cpp)
  target_link_libraries(waxcpp_runtime_model_config_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_runtime_model_config_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_runtime_model_config_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_runtime_model_config_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_runtime_model_config_test COMMAND waxcpp_runtime_model_config_test)

  add_executable(waxcpp_index_job_manager_test
    tests/unit/index_job_manager_test.cpp
    server/index_job_manager.cpp
    server/server_utils.cpp
  )
  target_link_libraries(waxcpp_index_job_manager_test PRIVATE waxcpp)
  target_include_directories(waxcpp_index_job_manager_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
  if (MSVC)
    target_compile_options(waxcpp_index_job_manager_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_index_job_manager_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_index_job_manager_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_index_job_manager_test COMMAND waxcpp_index_job_manager_test)

  add_executable(waxcpp_ue5_filesystem_scanner_test
    tests/unit/ue5_filesystem_scanner_test.cpp
    server/ue5_filesystem_scanner.cpp
    server/server_utils.cpp
  )
  target_link_libraries(waxcpp_ue5_filesystem_scanner_test PRIVATE waxcpp)
  target_include_directories(waxcpp_ue5_filesystem_scanner_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
  if (MSVC)
    target_compile_options(waxcpp_ue5_filesystem_scanner_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_ue5_filesystem_scanner_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_ue5_filesystem_scanner_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_ue5_filesystem_scanner_test COMMAND waxcpp_ue5_filesystem_scanner_test)

  add_executable(waxcpp_ue5_chunk_manifest_test
    tests/unit/ue5_chunk_manifest_test.cpp
    server/ue5_chunk_manifest.cpp
    server/bpl_json_compressor.cpp
    server/ue5_filesystem_scanner.cpp
    server/server_utils.cpp
  )
  target_link_libraries(waxcpp_ue5_chunk_manifest_test PRIVATE waxcpp Poco::JSON Poco::Foundation)
  target_include_directories(waxcpp_ue5_chunk_manifest_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
  if (MSVC)
    target_compile_options(waxcpp_ue5_chunk_manifest_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_ue5_chunk_manifest_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_ue5_chunk_manifest_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_ue5_chunk_manifest_test COMMAND waxcpp_ue5_chunk_manifest_test)

  if (WAXCPP_BUILD_SERVER AND
      TARGET Poco::Foundation AND
      TARGET Poco::Net AND
      TARGET Poco::JSON)
    add_executable(waxcpp_llama_cpp_generation_client_test
      tests/unit/llama_cpp_generation_client_test.cpp
      server/llama_cpp_generation_client.cpp
      server/server_utils.cpp
    )
    target_link_libraries(waxcpp_llama_cpp_generation_client_test PRIVATE
      waxcpp
      Poco::Net
      $<$<PLATFORM_ID:Windows>:Poco::NetSSLWin>
      $<$<NOT:$<PLATFORM_ID:Windows>>:Poco::NetSSL>
      Poco::JSON
      Poco::Foundation
    )
    target_include_directories(waxcpp_llama_cpp_generation_client_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
    if (MSVC)
      target_compile_options(waxcpp_llama_cpp_generation_client_test PRIVATE /W4 /permissive-)
    else()
      target_compile_options(waxcpp_llama_cpp_generation_client_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME waxcpp_llama_cpp_generation_client_test COMMAND waxcpp_llama_cpp_generation_client_test)

    add_executable(waxcpp_llama_cpp_embedding_provider_test
      tests/unit/llama_cpp_embedding_provider_test.cpp
      server/llama_cpp_embedding_provider.cpp
      server/server_utils.cpp
    )
    target_link_libraries(waxcpp_llama_cpp_embedding_provider_test PRIVATE
      waxcpp
      Poco::Net
      Poco::JSON
      Poco::Foundation
    )
    target_include_directories(waxcpp_llama_cpp_embedding_provider_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
    if (MSVC)
      target_compile_options(waxcpp_llama_cpp_embedding_provider_test PRIVATE /W4 /permissive-)
    else()
      target_compile_options(waxcpp_llama_cpp_embedding_provider_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME waxcpp_llama_cpp_embedding_provider_test COMMAND waxcpp_llama_cpp_embedding_provider_test)

    add_executable(waxcpp_wax_rag_handler_index_test
      tests/unit/wax_rag_handler_index_test.cpp
      server/fact_service.cpp
      server/wax_rag_handler.cpp
      server/index_job_manager.cpp
      server/ue5_filesystem_scanner.cpp
      server/ue5_chunk_manifest.cpp
      server/bpl_json_compressor.cpp
      server/bpl_json_patch_compiler.cpp
      server/bpl_structural_enricher.cpp
      server/llama_cpp_generation_client.cpp
      server/llama_cpp_embedding_provider.cpp
      server/regex_ue5_enricher.cpp
      server/llm_fact_enricher.cpp
      server/runtime_config.cpp
      server/server_utils.cpp
    )
    target_link_libraries(waxcpp_wax_rag_handler_index_test PRIVATE
      waxcpp
      Poco::Net
      $<$<PLATFORM_ID:Windows>:Poco::NetSSLWin>
      $<$<NOT:$<PLATFORM_ID:Windows>>:Poco::NetSSL>
      Poco::JSON
      Poco::Foundation
    )
    target_include_directories(waxcpp_wax_rag_handler_index_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
    if (MSVC)
      target_compile_options(waxcpp_wax_rag_handler_index_test PRIVATE /W4 /permissive-)
    else()
      target_compile_options(waxcpp_wax_rag_handler_index_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME waxcpp_wax_rag_handler_index_test COMMAND waxcpp_wax_rag_handler_index_test)

    add_executable(waxcpp_wax_rag_handler_answer_test
      tests/unit/wax_rag_handler_answer_test.cpp
      server/fact_service.cpp
      server/wax_rag_handler.cpp
      server/index_job_manager.cpp
      server/ue5_filesystem_scanner.cpp
      server/ue5_chunk_manifest.cpp
      server/bpl_json_compressor.cpp
      server/bpl_json_patch_compiler.cpp
      server/bpl_structural_enricher.cpp
      server/llama_cpp_generation_client.cpp
      server/llama_cpp_embedding_provider.cpp
      server/regex_ue5_enricher.cpp
      server/llm_fact_enricher.cpp
      server/runtime_config.cpp
      server/server_utils.cpp
    )
    target_link_libraries(waxcpp_wax_rag_handler_answer_test PRIVATE
      waxcpp
      Poco::Net
      $<$<PLATFORM_ID:Windows>:Poco::NetSSLWin>
      $<$<NOT:$<PLATFORM_ID:Windows>>:Poco::NetSSL>
      Poco::JSON
      Poco::Foundation
    )
    target_include_directories(waxcpp_wax_rag_handler_answer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
    if (MSVC)
      target_compile_options(waxcpp_wax_rag_handler_answer_test PRIVATE /W4 /permissive-)
    else()
      target_compile_options(waxcpp_wax_rag_handler_answer_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME waxcpp_wax_rag_handler_answer_test COMMAND waxcpp_wax_rag_handler_answer_test)

    add_executable(waxcpp_regex_ue5_enricher_test
      tests/unit/regex_ue5_enricher_test.cpp
      server/regex_ue5_enricher.cpp
      server/ue5_chunk_manifest.cpp
      server/bpl_json_compressor.cpp
      server/ue5_filesystem_scanner.cpp
      server/server_utils.cpp
    )
    target_link_libraries(waxcpp_regex_ue5_enricher_test PRIVATE waxcpp Poco::JSON Poco::Foundation)
    target_include_directories(waxcpp_regex_ue5_enricher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
    if (MSVC)
      target_compile_options(waxcpp_regex_ue5_enricher_test PRIVATE /W4 /permissive-)
    else()
      target_compile_options(waxcpp_regex_ue5_enricher_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME waxcpp_regex_ue5_enricher_test COMMAND waxcpp_regex_ue5_enricher_test)

    add_executable(waxcpp_llm_fact_enricher_test
      tests/unit/llm_fact_enricher_test.cpp
      server/llm_fact_enricher.cpp
      server/llama_cpp_generation_client.cpp
      server/ue5_chunk_manifest.cpp
      server/bpl_json_compressor.cpp
      server/ue5_filesystem_scanner.cpp
      server/server_utils.cpp
    )
    target_link_libraries(waxcpp_llm_fact_enricher_test PRIVATE
      waxcpp
      Poco::Net
      $<$<PLATFORM_ID:Windows>:Poco::NetSSLWin>
      $<$<NOT:$<PLATFORM_ID:Windows>>:Poco::NetSSL>
      Poco::JSON
      Poco::Foundation
    )
    target_include_directories(waxcpp_llm_fact_enricher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
    if (MSVC)
      target_compile_options(waxcpp_llm_fact_enricher_test PRIVATE /W4 /permissive-)
    else()
      target_compile_options(waxcpp_llm_fact_enricher_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME waxcpp_llm_fact_enricher_test COMMAND waxcpp_llm_fact_enricher_test)

    add_executable(waxcpp_bpl_json_compressor_test
      tests/unit/bpl_json_compressor_test.cpp
      server/bpl_json_compressor.cpp
    )
    target_link_libraries(waxcpp_bpl_json_compressor_test PRIVATE
      waxcpp
      Poco::JSON
      Poco::Foundation
    )
    target_include_directories(waxcpp_bpl_json_compressor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
    if (MSVC)
      target_compile_options(waxcpp_bpl_json_compressor_test PRIVATE /W4 /permissive-)
    else()
      target_compile_options(waxcpp_bpl_json_compressor_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME waxcpp_bpl_json_compressor_test COMMAND waxcpp_bpl_json_compressor_test)

    add_executable(waxcpp_bpl_json_patch_compiler_test
      tests/unit/bpl_json_patch_compiler_test.cpp
      server/bpl_json_patch_compiler.cpp
    )
    target_link_libraries(waxcpp_bpl_json_patch_compiler_test PRIVATE
      waxcpp
      Poco::JSON
      Poco::Foundation
      Poco::Util
    )
    target_include_directories(waxcpp_bpl_json_patch_compiler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
    if (MSVC)
      target_compile_options(waxcpp_bpl_json_patch_compiler_test PRIVATE /W4 /permissive-)
    else()
      target_compile_options(waxcpp_bpl_json_patch_compiler_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME waxcpp_bpl_json_patch_compiler_test COMMAND waxcpp_bpl_json_patch_compiler_test)

    add_executable(waxcpp_bpl_structural_enricher_test
      tests/unit/bpl_structural_enricher_test.cpp
      server/bpl_structural_enricher.cpp
    )
    target_link_libraries(waxcpp_bpl_structural_enricher_test PRIVATE
      waxcpp
      Poco::JSON
      Poco::Foundation
    )
    target_include_directories(waxcpp_bpl_structural_enricher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
    if (MSVC)
      target_compile_options(waxcpp_bpl_structural_enricher_test PRIVATE /W4 /permissive-)
    else()
      target_compile_options(waxcpp_bpl_structural_enricher_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME waxcpp_bpl_structural_enricher_test COMMAND waxcpp_bpl_structural_enricher_test)
  endif()

  add_executable(waxcpp_structured_memory_store_test tests/unit/structured_memory_store_test.cpp)
  target_link_libraries(waxcpp_structured_memory_store_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_structured_memory_store_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_structured_memory_store_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_structured_memory_store_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_structured_memory_store_test COMMAND waxcpp_structured_memory_store_test)

  add_executable(waxcpp_wal_ring_test tests/unit/wal_ring_test.cpp)
  target_link_libraries(waxcpp_wal_ring_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_wal_ring_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_wal_ring_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_wal_ring_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_wal_ring_test COMMAND waxcpp_wal_ring_test)

  add_executable(waxcpp_wal_ring_writer_test tests/unit/wal_ring_writer_test.cpp)
  target_link_libraries(waxcpp_wal_ring_writer_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_wal_ring_writer_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_wal_ring_writer_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_wal_ring_writer_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_wal_ring_writer_test COMMAND waxcpp_wal_ring_writer_test)

  add_executable(waxcpp_query_analyzer_test tests/unit/query_analyzer_test.cpp)
  target_link_libraries(waxcpp_query_analyzer_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_query_analyzer_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_query_analyzer_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_query_analyzer_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_query_analyzer_test COMMAND waxcpp_query_analyzer_test)

  add_executable(waxcpp_token_counter_test tests/unit/token_counter_test.cpp)
  target_link_libraries(waxcpp_token_counter_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_token_counter_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_token_counter_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_token_counter_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_token_counter_test COMMAND waxcpp_token_counter_test)

  add_executable(waxcpp_surrogate_generator_test tests/unit/surrogate_generator_test.cpp)
  target_link_libraries(waxcpp_surrogate_generator_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_surrogate_generator_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_surrogate_generator_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_surrogate_generator_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_surrogate_generator_test COMMAND waxcpp_surrogate_generator_test)

  add_executable(waxcpp_maintenance_test tests/unit/maintenance_test.cpp)
  target_link_libraries(waxcpp_maintenance_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_maintenance_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_maintenance_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_maintenance_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_maintenance_test COMMAND waxcpp_maintenance_test)

  add_executable(waxcpp_access_stats_test tests/unit/access_stats_test.cpp)
  target_link_libraries(waxcpp_access_stats_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_access_stats_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_access_stats_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_access_stats_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_access_stats_test COMMAND waxcpp_access_stats_test)

  add_executable(waxcpp_importance_scorer_test tests/unit/importance_scorer_test.cpp)
  target_link_libraries(waxcpp_importance_scorer_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_importance_scorer_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_importance_scorer_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_importance_scorer_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_importance_scorer_test COMMAND waxcpp_importance_scorer_test)

  add_executable(waxcpp_answer_extractor_test tests/unit/answer_extractor_test.cpp)
  target_link_libraries(waxcpp_answer_extractor_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_answer_extractor_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_answer_extractor_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_answer_extractor_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_answer_extractor_test COMMAND waxcpp_answer_extractor_test)

  add_executable(waxcpp_text_chunker_test tests/unit/text_chunker_test.cpp)
  target_link_libraries(waxcpp_text_chunker_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_text_chunker_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_text_chunker_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_text_chunker_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_text_chunker_test COMMAND waxcpp_text_chunker_test)

  add_executable(waxcpp_intent_rerank_test tests/unit/intent_rerank_test.cpp)
  target_link_libraries(waxcpp_intent_rerank_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_intent_rerank_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_intent_rerank_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_intent_rerank_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_intent_rerank_test COMMAND waxcpp_intent_rerank_test)

  add_executable(waxcpp_tier_selector_test tests/unit/tier_selector_test.cpp)
  target_link_libraries(waxcpp_tier_selector_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_tier_selector_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_tier_selector_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_tier_selector_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_tier_selector_test COMMAND waxcpp_tier_selector_test)

  add_executable(waxcpp_embedding_memoizer_test tests/unit/embedding_memoizer_test.cpp)
  target_link_libraries(waxcpp_embedding_memoizer_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_embedding_memoizer_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_embedding_memoizer_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_embedding_memoizer_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_embedding_memoizer_test COMMAND waxcpp_embedding_memoizer_test)

  add_executable(waxcpp_vector_math_test tests/unit/vector_math_test.cpp)
  target_link_libraries(waxcpp_vector_math_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_vector_math_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_vector_math_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_vector_math_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_vector_math_test COMMAND waxcpp_vector_math_test)

  add_executable(waxcpp_live_set_rewrite_test tests/unit/live_set_rewrite_test.cpp)
  target_link_libraries(waxcpp_live_set_rewrite_test PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_live_set_rewrite_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_live_set_rewrite_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_live_set_rewrite_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_live_set_rewrite_test COMMAND waxcpp_live_set_rewrite_test)

  add_executable(waxcpp_mv2s_parity_test tests/parity/mv2s_fixture_parity_test.cpp)
  target_link_libraries(waxcpp_mv2s_parity_test PRIVATE waxcpp)
  target_compile_definitions(waxcpp_mv2s_parity_test PRIVATE
    WAXCPP_PARITY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/parity"
    WAXCPP_REQUIRE_PARITY_FIXTURES=$<IF:$<BOOL:${WAXCPP_REQUIRE_PARITY_FIXTURES}>,1,0>
    WAXCPP_REQUIRE_SWIFT_FIXTURES=$<IF:$<BOOL:${WAXCPP_REQUIRE_SWIFT_FIXTURES}>,1,0>
  )
  if (MSVC)
    target_compile_options(waxcpp_mv2s_parity_test PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_mv2s_parity_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_mv2s_parity_test REUSE_FROM waxcpp)
  add_test(NAME waxcpp_mv2s_parity_test COMMAND waxcpp_mv2s_parity_test)

  add_executable(waxcpp_mv2s_fixture_generator tests/parity/mv2s_fixture_generator.cpp)
  target_link_libraries(waxcpp_mv2s_fixture_generator PRIVATE waxcpp)
  target_compile_definitions(waxcpp_mv2s_fixture_generator PRIVATE
    WAXCPP_PARITY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/parity/synthetic"
  )
  if (MSVC)
    target_compile_options(waxcpp_mv2s_fixture_generator PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_mv2s_fixture_generator PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_mv2s_fixture_generator REUSE_FROM waxcpp)

  # mv2s binary analyzer tool
  add_executable(waxcpp_mv2s_analyzer tools/mv2s_analyzer.cpp)
  target_link_libraries(waxcpp_mv2s_analyzer PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_mv2s_analyzer PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_mv2s_analyzer PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_mv2s_analyzer REUSE_FROM waxcpp)

  # mv2s vacuum (compaction) tool
  add_executable(waxcpp_mv2s_vacuum tools/mv2s_vacuum.cpp)
  target_link_libraries(waxcpp_mv2s_vacuum PRIVATE waxcpp)
  if (MSVC)
    target_compile_options(waxcpp_mv2s_vacuum PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_mv2s_vacuum PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_precompile_headers(waxcpp_mv2s_vacuum REUSE_FROM waxcpp)

  # Blueprint structural enricher — CLI wrapper over server/bpl_structural_enricher
  add_executable(waxcpp_bpl_structural_enricher_poc
    tools/bpl_structural_enricher_poc.cpp
    server/bpl_structural_enricher.cpp
  )
  target_link_libraries(waxcpp_bpl_structural_enricher_poc PRIVATE
    waxcpp Poco::JSON Poco::Foundation)
  target_include_directories(waxcpp_bpl_structural_enricher_poc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
  if (MSVC)
    target_compile_options(waxcpp_bpl_structural_enricher_poc PRIVATE /W4 /permissive-)
  else()
    target_compile_options(waxcpp_bpl_structural_enricher_poc PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if (MSVC)
  get_property(_waxcpp_root_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_target IN LISTS _waxcpp_root_targets)
    if (_target MATCHES "^waxcpp")
      waxcpp_apply_debug_suffix("${_target}")
    endif()
  endforeach()

  if (WAXCPP_BUILD_SERVER)
    get_property(_waxcpp_server_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/server" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _waxcpp_server_targets)
      if (_target MATCHES "^waxcpp")
        waxcpp_apply_debug_suffix("${_target}")
      endif()
    endforeach()
  endif()
endif()

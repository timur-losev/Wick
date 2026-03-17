option(WAXCPP_BUILD_RAG_SERVER "Build waxcpp_rag_server HTTP bridge" ON)
option(WAXCPP_SERVER_BUILD_FROM_SUBMODULE_POCO
  "Build Poco from third_party/poco if Poco package is not found"
  ON)

if (NOT WAXCPP_BUILD_RAG_SERVER)
  return()
endif()

find_package(Poco CONFIG QUIET COMPONENTS Foundation Net Util JSON XML)

if (NOT TARGET Poco::Foundation AND WAXCPP_SERVER_BUILD_FROM_SUBMODULE_POCO)
  if (EXISTS "${CMAKE_SOURCE_DIR}/third_party/poco/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/poco"
                     "${CMAKE_BINARY_DIR}/third_party/poco"
                     EXCLUDE_FROM_ALL)
  endif()
endif()

if (NOT TARGET Poco::Foundation OR
    NOT TARGET Poco::Net OR
    NOT TARGET Poco::Util OR
    NOT TARGET Poco::JSON OR
    NOT TARGET Poco::XML)
  message(FATAL_ERROR
    "waxcpp_rag_server requires Poco targets (Foundation, Net, Util, JSON, XML).\n"
    "Provide Poco via package config or initialize/build third_party/poco.")
endif()

add_executable(waxcpp_rag_server
  fact_service.cpp
  index_job_manager.cpp
  llama_cpp_generation_client.cpp
  llama_cpp_embedding_provider.cpp
  llm_fact_enricher.cpp
  main.cpp
  json_rpc.cpp
  regex_ue5_enricher.cpp
  runtime_config.cpp
  server_utils.cpp
  ue5_chunk_manifest.cpp
  ue5_filesystem_scanner.cpp
  wax_rag_handler.cpp
)

target_include_directories(waxcpp_rag_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(waxcpp_rag_server PRIVATE
  waxcpp
  Poco::Net
  Poco::Util
  Poco::JSON
  Poco::XML
  Poco::Foundation
)

if (MSVC)
  target_compile_options(waxcpp_rag_server PRIVATE /W4 /permissive-)
  target_link_libraries(waxcpp_rag_server PRIVATE Psapi.lib)
else()
  target_compile_options(waxcpp_rag_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS waxcpp_rag_server
  RUNTIME DESTINATION bin
)

set(GW_PUBLIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/public)
set(GW_PRIVATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/private)

function(gw_module target)
    file(GLOB_RECURSE srcs CONFIGURE_DEPENDS ${GW_PRIVATE_DIR}/${target}/*.cpp)
    if(NOT srcs)
        set(stub ${CMAKE_CURRENT_BINARY_DIR}/gw_${target}_stub.cpp)
        file(WRITE ${stub} "namespace gw { void gw_${target}_stub() {} }\n")
        set(srcs ${stub})
    endif()
    add_library(gw_${target} STATIC ${srcs})
    target_include_directories(gw_${target} PUBLIC ${GW_PUBLIC_DIR})
    gw_add_warnings(gw_${target})
endfunction()

gw_module(base)
target_link_libraries(gw_base PUBLIC spdlog::spdlog)

gw_module(net)
target_link_libraries(gw_net PUBLIC gw_base Threads::Threads)

gw_module(http)
target_link_libraries(gw_http PUBLIC gw_net)

gw_module(store)
target_link_libraries(gw_store PUBLIC gw_net PkgConfig::PQ)

gw_module(proto)
target_link_libraries(gw_proto PUBLIC gw_base nlohmann_json)

gw_module(gateway)
target_link_libraries(gw_gateway PUBLIC gw_http gw_store gw_proto OpenSSL::Crypto)

file(GLOB_RECURSE GW_APP_SRC CONFIGURE_DEPENDS ${GW_PRIVATE_DIR}/app/*.cpp)
add_executable(llm-gateway ${GW_APP_SRC})
target_link_libraries(llm-gateway PRIVATE gw_gateway CLI11::CLI11)
gw_add_warnings(llm-gateway)

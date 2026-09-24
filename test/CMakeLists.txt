include(GoogleTest)

file(GLOB GW_UNIT_TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp)

if(GW_UNIT_TEST_SRCS)
    add_executable(gateway_unit_tests ${GW_UNIT_TEST_SRCS})
    target_link_libraries(gateway_unit_tests PRIVATE
        gw_gateway gw_store gw_http gw_net gw_base gw_proto
        GTest::gtest_main
    )
    target_include_directories(gateway_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(gateway_unit_tests PRIVATE
        GW_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/unit/data")
    gw_add_warnings(gateway_unit_tests)
    if(CMAKE_CXX_FLAGS MATCHES "sanitize=thread")
        # aarch64 + 高 mmap_rnd_bits 下 TSan 需要关闭 ASLR；discovery 也要走 launcher，
        # gtest_discover_tests 的 TEST_LAUNCHER 不覆盖 discovery，这里退化成单个测试。
        add_test(NAME gateway_unit_tests
            COMMAND setarch ${CMAKE_SYSTEM_PROCESSOR} -R $<TARGET_FILE:gateway_unit_tests>)
    else()
        gtest_discover_tests(gateway_unit_tests DISCOVERY_MODE PRE_TEST)
    endif()
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/fault_upstream/main.cpp)
    add_executable(fault-upstream ${CMAKE_CURRENT_SOURCE_DIR}/fault_upstream/main.cpp)
    target_link_libraries(fault-upstream PRIVATE gw_http gw_proto CLI11::CLI11)
    gw_add_warnings(fault-upstream)
endif()

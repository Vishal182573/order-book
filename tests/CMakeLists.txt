if(NOT GTest_FOUND AND NOT TARGET GTest::gtest)
  # Already fetched in root CMakeLists
endif()

add_executable(test_order_book test_order_book.cpp)

target_include_directories(test_order_book PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_order_book PRIVATE
    GTest::gtest_main
    Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(test_order_book)

add_executable(bench_order_book bench_order_book.cpp)

target_include_directories(bench_order_book PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(bench_order_book PRIVATE
    benchmark::benchmark
    Threads::Threads
)

target_compile_options(bench_order_book PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-march=native -O3>
)

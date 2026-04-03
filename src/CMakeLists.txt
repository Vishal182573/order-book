add_executable(order_book_engine main.cpp)

target_include_directories(order_book_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(order_book_engine PRIVATE Threads::Threads)

target_compile_options(order_book_engine PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-march=native -mtune=native>
    $<$<CXX_COMPILER_ID:GNU>:-fno-omit-frame-pointer>
)

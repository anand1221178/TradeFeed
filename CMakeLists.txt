cmake_minimum_required(VERSION 3.20)
project(TradeFeed LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# --- Exchange binary ---
add_executable(tradefeed
    src/main.cpp
    src/core/order_book.cpp
    src/core/matching_engine.cpp
)
target_include_directories(tradefeed PRIVATE src)
target_compile_options(tradefeed PRIVATE -O3 -march=native -Wall -Wextra -Wpedantic)
target_link_libraries(tradefeed PRIVATE Threads::Threads)

# --- Order book + engine benchmark ---
add_executable(bench_orderbook
    bench/order_book_bench.cpp
    src/core/order_book.cpp
    src/core/matching_engine.cpp
)
target_include_directories(bench_orderbook PRIVATE src)
target_compile_options(bench_orderbook PRIVATE -O3 -march=native -Wall -Wextra)
target_link_libraries(bench_orderbook PRIVATE Threads::Threads)

# --- Ring buffer benchmark ---
add_executable(bench_ring
    bench/ring_buffer_bench.cpp
)
target_include_directories(bench_ring PRIVATE src)
target_compile_options(bench_ring PRIVATE -O3 -march=native -Wall -Wextra)
target_link_libraries(bench_ring PRIVATE Threads::Threads)

# --- Alpaca market data feed (optional: needs OpenSSL) ---
# Built as a separate binary so the exchange keeps zero external dependencies.
find_package(OpenSSL)
if(OpenSSL_FOUND)
    add_executable(tradefeed-feed
        src/feed/feed_main.cpp
        src/feed/alpaca_ws.cpp
    )
    target_include_directories(tradefeed-feed PRIVATE src)
    target_compile_options(tradefeed-feed PRIVATE -O2 -Wall -Wextra)
    target_link_libraries(tradefeed-feed PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
else()
    message(STATUS "OpenSSL not found - skipping tradefeed-feed (Alpaca feed)")
endif()

# --- Feed normaliser self-test (no network, no credentials needed) ---
add_executable(feed_test src/feed/feed_test.cpp)
target_include_directories(feed_test PRIVATE src)
target_compile_options(feed_test PRIVATE -O1 -g -Wall -Wextra)
enable_testing()
add_test(NAME feed_test COMMAND feed_test)

# --- Debug builds with sanitizers ---
add_executable(tradefeed_debug
    src/main.cpp
    src/core/order_book.cpp
    src/core/matching_engine.cpp
)
target_include_directories(tradefeed_debug PRIVATE src)
target_compile_options(tradefeed_debug PRIVATE
    -O1 -g3 -fno-omit-frame-pointer -march=native
    -fsanitize=address,undefined
    -Wall -Wextra -Wpedantic
)
target_link_options(tradefeed_debug PRIVATE -fsanitize=address,undefined)
target_link_libraries(tradefeed_debug PRIVATE Threads::Threads)

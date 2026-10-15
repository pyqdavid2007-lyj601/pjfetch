add_executable(wget-curl
    main.cpp
    cli.hpp cli.cpp
    console.hpp console.cpp
    config.hpp config.cpp
    progress_view.hpp progress_view.cpp
    filename.hpp filename.cpp)
target_link_libraries(wget-curl PRIVATE libpjfetch::libpjfetch)
target_include_directories(wget-curl PRIVATE ${CMAKE_SOURCE_DIR}/src/libpjfetch/include)
target_compile_options(wget-curl PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W3;/permissive->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra>)

install(TARGETS wget-curl RUNTIME DESTINATION bin COMPONENT pjfetch)

add_library(libpjfetch SHARED
    include/pjfetch/export.h
    include/pjfetch/pjfetch.h
    src/downloader_impl.hpp
    src/downloader_impl.cpp
    src/probe.cpp
    src/planner.cpp
    src/segment_job.cpp
    src/file_writer.cpp
    src/meta.cpp
    src/errc_map.cpp
    src/session_factory.cpp)
add_library(libpjfetch::libpjfetch ALIAS libpjfetch)
set_target_properties(libpjfetch PROPERTIES OUTPUT_NAME "libpjfetch" PREFIX "")
target_compile_definitions(libpjfetch PRIVATE PJFETCH_BUILDING)
target_include_directories(libpjfetch PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(libpjfetch PRIVATE ${CMAKE_SOURCE_DIR}/src/libparajobs/include)
target_link_libraries(libpjfetch PRIVATE libparajobs::libparajobs cpr::cpr)
target_compile_options(libpjfetch PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4;/permissive->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>)

# 诊断开关：生成链接映射表（便于把崩溃 RVA 解析成函数名）
option(PJF_MAP "generate linker map for libpjfetch" OFF)
if(PJF_MAP AND MSVC)
    target_link_options(libpjfetch PRIVATE /MAP)
endif()

# 安装：同上
install(TARGETS libpjfetch
        RUNTIME DESTINATION bin COMPONENT pjfetch
        ARCHIVE DESTINATION lib COMPONENT pjfetch)
install(DIRECTORY include/pjfetch DESTINATION include COMPONENT pjfetch)

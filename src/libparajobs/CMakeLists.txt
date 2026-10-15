add_library(libparajobs SHARED
    include/parajobs/export.h
    include/parajobs/parajobs.h
    src/internal.hpp
    src/engine.cpp)
add_library(libparajobs::libparajobs ALIAS libparajobs)
set_target_properties(libparajobs PROPERTIES OUTPUT_NAME "libparajobs" PREFIX "")
target_compile_definitions(libparajobs PRIVATE PARAOBS_BUILDING)
target_include_directories(libparajobs PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# libgo 公共头目录（仅私有，coroutine.h 绝不出现在公共头）
target_include_directories(libparajobs PRIVATE ${CMAKE_SOURCE_DIR}/3rd-party/libgo/libgo)
# libgo vendor 补丁把 cmake_config.h 移入构建树（见 3rd-party/libgo/CMakeLists.txt）
target_include_directories(libparajobs PRIVATE ${CMAKE_BINARY_DIR}/generated/libgo/common)
target_link_libraries(libparajobs PRIVATE libgo_static)
if(WIN32)
    # libgo 的 winsock 钩子/反应堆直接调用 ws2_32 符号；静态库按需拉取，须显式链接。
    target_link_libraries(libparajobs PRIVATE ws2_32)
endif()
# 注意：本目标刻意不开 /permissive-。libgo 的 context/fiber/context.h 是老式 MSVC 成员限定
# 写法，在该模式下 C4596 会由警告升级为 error；消费第三方头不应启用严格一致性模式。
target_compile_options(libparajobs PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>)

# 安装：DLL 进 bin（三个产物必须同目录，见根 CMakeLists 的说明），导入库进 lib，契约头进 include
# COMPONENT pjfetch：只安装本组件，避免把 cpr 捆绑的 curl/zlib 也装进去
install(TARGETS libparajobs
        RUNTIME DESTINATION bin COMPONENT pjfetch
        ARCHIVE DESTINATION lib COMPONENT pjfetch)
install(DIRECTORY include/parajobs DESTINATION include COMPONENT pjfetch)

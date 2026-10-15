add_executable(pjb_smoke pjb_smoke.cpp)
target_link_libraries(pjb_smoke PRIVATE parajobs::parajobs)
target_include_directories(pjb_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src/parajobs/include)
# smoke 目标 include 的契约头含中文注释，MSVC 需显式 /utf-8 防 C4819
target_compile_options(pjb_smoke PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# fetch_smoke 已撤下：其外部 python 服务器会弹控制台窗口并继承父进程管道（ctest 等不到
# 管道 EOF 而挂死）；覆盖范围已由 PjfetchE2E 组完整涵盖，源文件保留作手工运行参考。

# 测试用 HTTP 服务器（独立进程）：以 GUI 子系统编译（WIN32_EXECUTABLE），操作系统不会为
# 它分配控制台，因此绝不可能弹窗；必须独立进程是因为 libgo 的 socket 钩子是进程级的，
# 同进程内的普通 socket 调用会落进为协程调度设计的钩子（未定义行为）。
add_executable(pjf_test_server pjf_test_server.cpp http_server_impl.hpp)
set_target_properties(pjf_test_server PROPERTIES WIN32_EXECUTABLE TRUE)
if(WIN32)
    target_link_libraries(pjf_test_server PRIVATE ws2_32)
endif()
if(MSVC)
    # GUI 子系统 + main() -> 需显式指定 CRT 入口
    target_link_options(pjf_test_server PRIVATE /ENTRY:mainCRTStartup)
endif()
target_compile_options(pjf_test_server PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# CppUTest 单元测试（黑盒 C ABI + 白盒内部单元）。白盒把内部 .cpp 直接编进测试可执行
# 文件：这些函数不导出，与 libpjfetch.dll 里的同名副本互不冲突。errc_map.cpp 需要 cpr
# 头，故链接 cpr::cpr 以传导其 include 目录。
set(PJF_UNIT_SOURCES
    unit/main.cpp
    unit/test_parajobs.cpp
    unit/test_pjfetch_api.cpp
    unit/test_pjfetch_e2e.cpp
    unit/test_core_units.cpp
    unit/test_wget_units.cpp)

set(PJF_WHITEBOX_SOURCES
    ${CMAKE_SOURCE_DIR}/src/pjfetch/src/planner.cpp
    ${CMAKE_SOURCE_DIR}/src/pjfetch/src/meta.cpp
    ${CMAKE_SOURCE_DIR}/src/pjfetch/src/errc_map.cpp
    ${CMAKE_SOURCE_DIR}/src/pjfetch/src/file_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/wget-curl/cli.cpp
    ${CMAKE_SOURCE_DIR}/src/wget-curl/filename.cpp
    ${CMAKE_SOURCE_DIR}/src/wget-curl/config.cpp)

add_executable(pjfetch_tests ${PJF_UNIT_SOURCES} ${PJF_WHITEBOX_SOURCES}
    unit/test_server.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_spawn.hpp)

target_include_directories(pjfetch_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/pjfetch/include
    ${CMAKE_SOURCE_DIR}/src/parajobs/include
    ${CMAKE_SOURCE_DIR}/src/pjfetch/src          # downloader_impl.hpp（内部）
    ${CMAKE_SOURCE_DIR}/src/wget-curl            # cli.hpp / filename.hpp / config.hpp
    ${CMAKE_CURRENT_SOURCE_DIR})                 # test_spawn.hpp

target_compile_definitions(pjfetch_tests PRIVATE
    PJF_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    PJF_TEST_SERVER_EXE="$<TARGET_FILE:pjf_test_server>")

target_compile_options(pjfetch_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(pjfetch_tests PRIVATE
    pjfetch::pjfetch      # C ABI（导入库）
    parajobs::parajobs    # C ABI（导入库）
    cpr::cpr              # 仅为 errc_map.cpp 的 cpr 头/枚举
    CppUTest::CppUTest)

# 测试服务器走独立进程（见 test_server.hpp），故测试目标无需 winsock

# CTest 注册。单元测试由下方 cpputest_discover_tests 按 TEST 粒度逐项注册，不重复注册整体
add_test(NAME smoke_parajobs COMMAND pjb_smoke)

# 超时保护：任何用例挂起都必须以失败告终，绝不允许无限阻塞 CI/验收
set_tests_properties(smoke_parajobs PROPERTIES TIMEOUT 180)

# CppUTest 的顶层已把 cmake/Modules 推入 CMAKE_MODULE_PATH
if(COMMAND include)
    include(CppUTest OPTIONAL RESULT_VARIABLE _pjf_cpputest_module)
    if(_pjf_cpputest_module AND COMMAND cpputest_discover_tests)
        cpputest_discover_tests(pjfetch_tests PROPERTIES TIMEOUT 180)
    endif()
endif()

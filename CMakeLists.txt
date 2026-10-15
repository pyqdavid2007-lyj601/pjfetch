cmake_minimum_required(VERSION 3.28)
project(pjfetch VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 与 libgo 硬编码 /MT[d] 匹配：全程单一静态 CRT（含 wget-curl 自包含、Detours 单 CRT 实例）。
# 仅 MSVC 生效；GCC/Clang 走工具链默认。
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
option(PJFETCH_BUILD_TOOL "Build wget-curl.exe" ON)
option(PJFETCH_BUILD_TESTS "Build smoke tests" ON)
option(PJFETCH_DISABLE_HOOK "libgo 关闭 winsock 钩子（协程退化为占线程）" OFF)

# --- third-party（先设变量再 add_subdirectory，CMP0077 normal-var 生效）---
set(BUILD_SHARED_LIBS OFF)          # libgo_static / cpr / curl 全静态
set(CPR_CURL_USE_LIBPSL OFF)        # 免 meson
set(CPR_ENABLE_SSL ON)              # WIN32 -> Schannel
set(CPR_ENABLE_INSTALL OFF)
set(CPR_BUILD_TESTS OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)   # libgo 的 cmake_minimum_required(2.8)
# PJFETCH_DISABLE_HOOK -> libgo 的 DISABLE_HOOK。libgo 在老策略下 option() 只认缓存变量，
# 普通 set 会被其 option() 覆盖，因此直接写缓存（FORCE 使其始终跟随我们的开关）。
set(DISABLE_HOOK "${PJFETCH_DISABLE_HOOK}" CACHE BOOL "libgo: disable hook (mapped from PJFETCH_DISABLE_HOOK)" FORCE)

# --- 对 libgo 的必需补丁（幂等应用）---
# 补丁内容：把生成的 cmake_config.h 写入构建树（而非第三方源码树），避免多套构建树
# 互相覆盖，并同步调整包含路径。以补丁文件而非"手工改动子模组"的方式保存，
# 是为了让 `git submodule update` 丢弃本地改动后仍能在配置期自动恢复。
# 幂等：先试 --reverse --check（已应用则跳过），否则正向应用。
set(PJF_LIBGO_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/patches/libgo-0001-cmake-config-build-tree.patch")
set(PJF_LIBGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rd-party/libgo")
if(EXISTS "${PJF_LIBGO_PATCH}" AND EXISTS "${PJF_LIBGO_DIR}/CMakeLists.txt")
    execute_process(COMMAND git apply --check --reverse "${PJF_LIBGO_PATCH}"
                    WORKING_DIRECTORY "${PJF_LIBGO_DIR}"
                    RESULT_VARIABLE _pjf_patch_already
                    OUTPUT_QUIET ERROR_QUIET)
    if(NOT _pjf_patch_already EQUAL 0)
        execute_process(COMMAND git apply "${PJF_LIBGO_PATCH}"
                        WORKING_DIRECTORY "${PJF_LIBGO_DIR}"
                        RESULT_VARIABLE _pjf_patch_rc
                        OUTPUT_VARIABLE _pjf_patch_out
                        ERROR_VARIABLE _pjf_patch_err)
        if(_pjf_patch_rc EQUAL 0)
            message(STATUS "pjfetch: applied libgo patch (cmake_config.h -> build tree)")
        else()
            message(WARNING "pjfetch: 无法应用 libgo 补丁（${PJF_LIBGO_PATCH}）：${_pjf_patch_err}\n"
                            "          若已手工改过 3rd-party/libgo/CMakeLists.txt 可忽略此警告。")
        endif()
    endif()
endif()

add_subdirectory(3rd-party/libgo)        # -> libgo_static
unset(CMAKE_POLICY_VERSION_MINIMUM)
add_subdirectory(3rd-party/cpr)          # -> cpr::cpr（首次 configure 需联网下载 curl tarball）

# --- CppUTest 4.0（单元测试框架）---
# 关键：必须关闭其内存泄漏检测。CppUTest 开启时会以 PUBLIC 编译选项对所有消费者
# 强制 /FI MemoryLeakDetectorForceInclude.h（该头 #define new/malloc/free），并覆盖全局
# operator new/delete —— 这会在 /MT 静态 CRT + libgo fiber + cpr/curl 头之下必然冲突；
# 且 /MT 下每个 DLL 有独立堆，跨模块跟踪本就无意义。
set(CPPUTEST_MEM_LEAK_DETECTION_DISABLED ON CACHE BOOL "leak detection off (see 根 CMakeLists 说明)" FORCE)
set(CPPUTEST_EXTENSIONS OFF CACHE BOOL "不构建 CppUTestExt（不需要 Mock）" FORCE)
set(CPPUTEST_BUILD_TESTING OFF CACHE BOOL "不构建 CppUTest 自带测试" FORCE)
set(CPPUTEST_EXAMPLES OFF CACHE BOOL "不构建 CppUTest 示例" FORCE)
add_subdirectory(3rd-party/cpputest ${CMAKE_BINARY_DIR}/cpputest)   # -> CppUTest（静态）

# 警告选项不设在根（避免污染第三方），由各 src/*/CMakeLists 对我方目标按编译器分支设置：
#   MSVC: /utf-8 /W4    GCC/Clang: -Wall -Wextra -Wpedantic
# 注意 parajobs 目标刻意不开 /permissive-（libgo 老式 MSVC 扩展，见 src/libparajobs/CMakeLists.txt）

# --- 运行时产物统一输出目录 ---
# 模块存在硬依赖链：wget-curl.exe -> libpjfetch.dll -> libparajobs.dll。
# Windows 加载器只在 EXE 所在目录与 PATH 中搜索依赖（不做递归子目录查找），
# 若各目标留在自己的子目录里，EXE 与测试程序会因找不到依赖 DLL 而无法启动。
# 统一放到 ${CMAKE_BINARY_DIR}/bin 一并解决（自测、分发、PATH 设置都简单）。
# 置于第三方 add_subdirectory 之后，仅作用于我方目标。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
# 多配置生成器（VS 生成器）需要逐配置后缀，单配置（Ninja）下这些为空后缀
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/bin)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/bin)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/lib)
endforeach()

add_subdirectory(src/libparajobs)   # SHARED, OUTPUT_NAME "libparajobs"
add_subdirectory(src/libpjfetch)    # SHARED, OUTPUT_NAME "libpjfetch"
if(PJFETCH_BUILD_TOOL)
  add_subdirectory(src/wget-curl)
endif()
if(PJFETCH_BUILD_TESTS)
  # 必须在顶层调用：否则各子目录的 add_test 不会进入根 CTestTestfile.cmake，
  # 从构建根执行 ctest 会报 "No tests were found"
  enable_testing()
  add_subdirectory(test)
endif()

# --- 安装：手册与文档 ---
install(FILES docs/man/wget-curl.1 DESTINATION share/man/man1 COMPONENT pjfetch)
install(FILES docs/man/libpjfetch.3 docs/man/libparajobs.3 DESTINATION share/man/man3 COMPONENT pjfetch)
install(DIRECTORY docs/html/ DESTINATION share/doc/pjfetch/html COMPONENT pjfetch)
install(FILES README.md DESTINATION share/doc/pjfetch COMPONENT pjfetch)

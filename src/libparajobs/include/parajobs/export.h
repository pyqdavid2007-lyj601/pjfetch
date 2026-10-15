#ifndef PARAOBS_EXPORT_H
#define PARAOBS_EXPORT_H

/* libparajobs 导出宏：Windows 按是否正在构建本库（PARAOBS_BUILDING）分 dllexport/dllimport；
 * 其他平台统一默认可见性（配合 -fvisibility=hidden 只导出带本宏的符号）。 */
#ifdef _WIN32
#  ifdef PARAOBS_BUILDING
#    define PARAOBS_API __declspec(dllexport)
#  else
#    define PARAOBS_API __declspec(dllimport)
#  endif
#else
#  define PARAOBS_API __attribute__((visibility("default")))
#endif

#endif /* PARAOBS_EXPORT_H */

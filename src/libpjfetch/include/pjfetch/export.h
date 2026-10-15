#ifndef PJFETCH_EXPORT_H
#define PJFETCH_EXPORT_H

/* 动态库导出宏：Windows 按 PJFETCH_BUILDING 分 dllexport/dllimport，其他平台默认可见性。 */
#ifdef _WIN32
#  ifdef PJFETCH_BUILDING
#    define PJFETCH_API __declspec(dllexport)
#  else
#    define PJFETCH_API __declspec(dllimport)
#  endif
#else
#  define PJFETCH_API __attribute__((visibility("default")))
#endif

#endif /* PJFETCH_EXPORT_H */

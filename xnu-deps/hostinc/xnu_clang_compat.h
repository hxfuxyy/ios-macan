#ifndef XNU_CLANG_COMPAT_H
#define XNU_CLANG_COMPAT_H
#if !defined(ASSEMBLER) && !defined(__ASSEMBLER__)
#if !__has_builtin(__builtin_xnu_type_signature)
#define XNU_NO_TYPE_BUILTINS 1
#define __builtin_xnu_type_signature(type) "12"
#define __builtin_xnu_type_summary(type) ((1U << 1) | (1U << 2))
#ifdef __cplusplus
#define __builtin_xnu_types_compatible(a,b) __is_same(a, b)
#else
#define __builtin_xnu_types_compatible(a,b) __builtin_types_compatible_p(a, b)
#endif
#endif
#endif
#endif

#ifndef _SYS_LINKER_SET_H_
#define _SYS_LINKER_SET_H_
#define __LX_SET_KEEP(sect,set,sym,qv) \
 static void const * qv __set_##set##_sym_##sym \
     __attribute__((__section__("__DATA," sect), __used__)) = &(sym)
#define __LX_SET_DROP(set,sym,qv) \
 static void const * qv __set_##set##_sym_##sym __attribute__((__unused__)) = &(sym)
#define __LX_SET_sysinit_set(set,sym,qv) __LX_SET_KEEP("lx_sysinit", set, sym, qv)
#define __LX_SET_sysuninit_set(set,sym,qv) __LX_SET_DROP(set, sym, qv)
#define __LX_SET_sysctl_set(set,sym,qv) __LX_SET_DROP(set, sym, qv)
#define __LX_SET_modmetadata_set(set,sym,qv) __LX_SET_DROP(set, sym, qv)
#define __MAKE_SET_QV(set,sym,qv) __LX_SET_ ##set(set, sym, qv)
#define __MAKE_SET(set,sym) __MAKE_SET_QV(set, sym, const)
#define TEXT_SET(set,sym) __MAKE_SET(set, sym)
#define DATA_SET(set,sym) __MAKE_SET(set, sym)
#define DATA_WSET(set,sym) __MAKE_SET_QV(set, sym, )
#define BSS_SET(set,sym) __MAKE_SET(set, sym)
#define ABS_SET(set,sym) __MAKE_SET(set, sym)
#define SET_ENTRY(set,sym) __MAKE_SET(set, sym)
#define SET_DECLARE(set,ptype) \
 extern ptype *__CONCAT(__start_set_,set); \
 extern ptype *__CONCAT(__stop_set_,set)
#define SET_BEGIN(set) (&__CONCAT(__start_set_,set))
#define SET_LIMIT(set) (&__CONCAT(__stop_set_,set))
#define SET_FOREACH(pvar,set) for (pvar = SET_BEGIN(set); pvar < SET_LIMIT(set); pvar++)
#define SET_ITEM(set,i) ((SET_BEGIN(set))[i])
#define SET_COUNT(set) (SET_LIMIT(set) - SET_BEGIN(set))
#endif

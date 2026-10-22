#ifndef _LX12_CLK_H
#define _LX12_CLK_H
struct clk;
static inline struct clk *devm_clk_get(void *dev, const char *id) { return NULL; }
static inline int clk_prepare_enable(struct clk *c) { return 0; }
static inline void clk_disable_unprepare(struct clk *c) { }
#endif

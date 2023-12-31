#include <asm/io.h>
#include <asm/page.h>

long cpu_l1c_status(void);
void cpu_icache_enable(void *info);
void cpu_icache_disable(void *info);
void cpu_dcache_enable(void *info);
void cpu_dcache_disable(void *info);
uint32_t cpu_l2c_ctl_status(void);
void cpu_l2c_disable(void);
#ifdef CONFIG_PICOCOM_PC805
void cpu_dma_wb_range_phy(phys_addr_t paddr, size_t size);
void cpu_dma_inval_range_phy(phys_addr_t paddr, size_t size);
#endif

long get_non_blocking_status(void);
long get_write_around_status(void);
void sbi_enable_non_blocking_load_store(void);
void sbi_disable_non_blocking_load_store(void);
void sbi_enable_write_around(void);
void sbi_disable_write_around(void);

void sbi_enable_l1i_cache(void);
void sbi_disable_l1i_cache(void);
void sbi_enable_l1d_cache(void);
void sbi_disable_l1d_cache(void);

void sbi_set_mcache_ctl(unsigned long input);
void sbi_set_mmisc_ctl(unsigned long input);

void cpu_dma_inval_range(void *info);
void cpu_dma_wb_range(void *info);
void cpu_l2c_inval_range(unsigned long pa, unsigned long size);
void cpu_l2c_wb_range(unsigned long pa, unsigned long size);

/*
 * struct andesv5_cache_info
 * The member of this struct is dupilcated to some content of struct cacheinfo
 * to reduce the latence of searching dcache inforamtion in andesv5/cache.c.
 * At current only dcache-line-size is needed. when the content of
 * andesv5_cache_info has been initilized by function fill_cpu_cache_info(),
 * member init_done is set as true
 */
struct andesv5_cache_info {
	bool init_done;
	int dcache_line_size;
};

struct range_info {
	unsigned long start;
	unsigned long end;
	struct page *page;
};

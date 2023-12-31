#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/cacheinfo.h>
#include <linux/sizes.h>
#include <linux/smp.h>
#include <linux/irqflags.h>
#include <asm/csr.h>
#include <asm/sbi.h>
#include <asm/io.h>
#include <asm/andesv5/proc.h>
#include <asm/andesv5/csr.h>
#ifdef CONFIG_PERF_EVENTS
#include <asm/perf_event.h>
#endif

#define MAX_CACHE_LINE_SIZE 256
#define EVSEL_MASK	0xff
#define SEL_PER_CTL	8
#define SEL_OFF(id)	(8 * (id % 8))

#ifndef CONFIG_PICOCOM_PC805
void __iomem *l2c_base;
#endif

DEFINE_PER_CPU(struct andesv5_cache_info, cpu_cache_info) = {
	.init_done = 0,
	.dcache_line_size = SZ_32
};
static void fill_cpu_cache_info(struct andesv5_cache_info *cpu_ci)
{
    int ncpu = get_cpu();
	struct cpu_cacheinfo *this_cpu_ci =
			get_cpu_cacheinfo(ncpu);
	struct cacheinfo *this_leaf = this_cpu_ci->info_list;
	unsigned int i = 0;

	for(; i< this_cpu_ci->num_leaves ; i++, this_leaf++)
		if(this_leaf->type == CACHE_TYPE_DATA) {
			cpu_ci->dcache_line_size = this_leaf->coherency_line_size;
		}
	cpu_ci->init_done = true;
    put_cpu();
}


inline int get_cache_line_size(void)
{
    int ncpu = get_cpu();
	struct andesv5_cache_info *cpu_ci =
		&per_cpu(cpu_cache_info, ncpu);

	if(unlikely(cpu_ci->init_done == false))
		fill_cpu_cache_info(cpu_ci);
    put_cpu();
	return cpu_ci->dcache_line_size;
}

#ifndef CONFIG_PICOCOM_PC805
static uint32_t cpu_l2c_get_cctl_status(void)
{
	return readl((void*)(l2c_base + L2C_REG_STATUS_OFFSET));
}
#endif

void cpu_dcache_wb_range(unsigned long start, unsigned long end, int line_size, struct page *page)
{
#ifndef CONFIG_PICOCOM_PC805
	int mhartid = get_cpu();
	unsigned long pa = page_to_phys(page);

	if(start & (~PAGE_MASK))
		pa += start & ~PAGE_MASK;

	while (end > start) {
		custom_csr_write(CCTL_REG_UCCTLBEGINADDR_NUM, start);
		custom_csr_write(CCTL_REG_UCCTLCOMMAND_NUM, CCTL_L1D_VA_WB);

		if (l2c_base) {
			writel(pa, (void*)(l2c_base + L2C_REG_CN_ACC_OFFSET(mhartid)));
			writel(CCTL_L2_PA_WB, (void*)(l2c_base + L2C_REG_CN_CMD_OFFSET(mhartid)));
			while ((cpu_l2c_get_cctl_status() & CCTL_L2_STATUS_CN_MASK(mhartid))
				!= CCTL_L2_STATUS_IDLE);
		}

		start += line_size;
		pa += line_size;
	}

    put_cpu();
#else
    get_cpu();
    unsigned long pa_start = page_to_phys(page);
    unsigned long pa_end;

    if(start & (~PAGE_MASK))
        pa_start += start & ~PAGE_MASK;
    pa_end = pa_start + (end - start);

    while (pa_end > pa_start) {
        custom_csr_write(CCTL_REG_UCCTLBEGINADDR_NUM, pa_start);
        custom_csr_write(CCTL_REG_UCCTLCOMMAND_NUM, CCTL_L1D_VA_WB);

        pa_start += line_size;
    }

    put_cpu();
#endif
}

void cpu_dcache_inval_range(unsigned long start, unsigned long end, int line_size, struct page *page)
{
#ifndef CONFIG_PICOCOM_PC805
	int mhartid = get_cpu();
	unsigned long pa = page_to_phys(page);

	if(start & (~PAGE_MASK))
		pa += start & ~PAGE_MASK;

	while (end > start) {
		custom_csr_write(CCTL_REG_UCCTLBEGINADDR_NUM, start);
		custom_csr_write(CCTL_REG_UCCTLCOMMAND_NUM, CCTL_L1D_VA_INVAL);

		if (l2c_base) {
			writel(pa, (void*)(l2c_base + L2C_REG_CN_ACC_OFFSET(mhartid)));
			writel(CCTL_L2_PA_INVAL, (void*)(l2c_base + L2C_REG_CN_CMD_OFFSET(mhartid)));
			while ((cpu_l2c_get_cctl_status() & CCTL_L2_STATUS_CN_MASK(mhartid))
				!= CCTL_L2_STATUS_IDLE);
		}

		start += line_size;
		pa += line_size;
	}

    put_cpu();
#else
    get_cpu();
    unsigned long pa_start = page_to_phys(page);
    unsigned long pa_end;

    if(start & (~PAGE_MASK))
        pa_start += start & ~PAGE_MASK;
    pa_end = pa_start + (end - start);

    while (pa_end > pa_start) {
        custom_csr_write(CCTL_REG_UCCTLBEGINADDR_NUM, start);
        custom_csr_write(CCTL_REG_UCCTLCOMMAND_NUM, CCTL_L1D_VA_INVAL);

        pa_start += line_size;
    }

    put_cpu();
#endif
}
void cpu_dma_inval_range(void *info)
{
        unsigned long flags;
        unsigned long line_size = get_cache_line_size();
	struct range_info *ri = info;
	unsigned long start = ri->start;
	unsigned long end = ri->end;
	unsigned long old_start = start;
	unsigned long old_end = end;
	char cache_buf[2][MAX_CACHE_LINE_SIZE]={0};

	if (unlikely(start == end))
		return;

	start = start & (~(line_size - 1));
	end = ((end + line_size - 1) & (~(line_size - 1)));

        local_irq_save(flags);
	if (unlikely(start != old_start)) {
		memcpy(&cache_buf[0][0], (void *)start, line_size);
	}
	if (unlikely(end != old_end)) {
		memcpy(&cache_buf[1][0], (void *)(old_end & (~(line_size - 1))), line_size);
	}
	cpu_dcache_inval_range(start, end, line_size, ri->page);
	if (unlikely(start != old_start)) {
		memcpy((void *)start, &cache_buf[0][0], (old_start & (line_size - 1)));
	}
	if (unlikely(end != old_end)) {
		memcpy((void *)(old_end + 1), &cache_buf[1][(old_end & (line_size - 1)) + 1], end - old_end - 1);
	}
        local_irq_restore(flags);

}
EXPORT_SYMBOL(cpu_dma_inval_range);

void cpu_dma_wb_range(void *info)
{
	unsigned long flags;
	unsigned long line_size = get_cache_line_size();
	struct range_info *ri = info;
	unsigned long start = ri->start;
	unsigned long end = ri->end;

        local_irq_save(flags);
	start = start & (~(line_size - 1));
	cpu_dcache_wb_range(start, end, line_size, ri->page);
        local_irq_restore(flags);
}
EXPORT_SYMBOL(cpu_dma_wb_range);

#ifdef CONFIG_PICOCOM_PC805
void cpu_dma_inval_range_phy(phys_addr_t paddr, size_t size)
{
    unsigned long flags;
    unsigned long line_size = get_cache_line_size();
    unsigned long start = (unsigned long)paddr;
    unsigned long end = start + size;

    if (unlikely(start == end))
        return;

    local_irq_save(flags);
    get_cpu();

    start = start & (~(line_size - 1));
    end = ((end + line_size - 1) & (~(line_size - 1)));

    while (end > start) {
        custom_csr_write(CCTL_REG_UCCTLBEGINADDR_NUM, start);
        custom_csr_write(CCTL_REG_UCCTLCOMMAND_NUM, CCTL_L1D_VA_INVAL);

        start += line_size;
    }

    put_cpu();
    local_irq_restore(flags);
}
EXPORT_SYMBOL(cpu_dma_inval_range_phy);

void cpu_dma_wb_range_phy(phys_addr_t paddr, size_t size)
{
    unsigned long flags;
    unsigned long line_size = get_cache_line_size();
    unsigned long start = (unsigned long)paddr;
    unsigned long end = start + size;

    if (unlikely(start == end))
        return;

    local_irq_save(flags);
    get_cpu();

    start = start & (~(line_size - 1));
    end = ((end + line_size - 1) & (~(line_size - 1)));

    while (end > start) {
        custom_csr_write(CCTL_REG_UCCTLBEGINADDR_NUM, start);
        custom_csr_write(CCTL_REG_UCCTLCOMMAND_NUM, CCTL_L1D_VA_WB);

        start += line_size;
    }

    put_cpu();
    local_irq_restore(flags);
}
EXPORT_SYMBOL(cpu_dma_wb_range_phy);
#endif

/*non-blocking load store*/
long get_non_blocking_status(void)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_GET_MMISC_CTL_STATUS, 0, 0, 0, 0, 0, 0);
	return ret.value;
}
void sbi_set_mcache_ctl(unsigned long input)
{
	unsigned long flags;

	local_irq_save(flags);
	sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_SET_MCACHE_CTL, input, 0, 0, 0, 0, 0);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(sbi_set_mcache_ctl);

void sbi_set_mmisc_ctl(unsigned long input)
{
       unsigned long flags;

       local_irq_save(flags);
       sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_NON_BLOCKING_LOAD_STORE, input, 0, 0, 0, 0, 0);
       local_irq_restore(flags);
}
EXPORT_SYMBOL(sbi_set_mmisc_ctl);

/*write around*/
long get_write_around_status(void)
{
	struct sbiret ret;
	ret = sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_GET_MCACHE_CTL_STATUS, 0, 0, 0, 0, 0, 0);
	return ret.value;
}


void sbi_enable_non_blocking_load_store(void)
{
       unsigned long flags;

       local_irq_save(flags);
       sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_WRITE_AROUND, 1, 0, 0, 0, 0, 0);
       local_irq_restore(flags);
}

void sbi_disable_non_blocking_load_store(void)
{
       unsigned long flags;

       local_irq_save(flags);
       sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_WRITE_AROUND, 0, 0, 0, 0, 0, 0);
       local_irq_restore(flags);
}

void sbi_enable_write_around(void)
{
	unsigned long flags;

       local_irq_save(flags);
       sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_SET_MCACHE_CTL, 1, 0, 0, 0, 0, 0);
       local_irq_restore(flags);
}

void sbi_disable_write_around(void)
{
       unsigned long flags;

       local_irq_save(flags);
       sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_SET_MMISC_CTL, 0, 0, 0, 0, 0, 0);
       local_irq_restore(flags);
}
/* L1 Cache Prefetch */

void sbi_enable_l1i_cache(void)
{
	unsigned long flags;

        local_irq_save(flags);
        sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_L1CACHE_I_PREFETCH, 1, 0, 0, 0, 0, 0);
        local_irq_restore(flags);
}

void sbi_disable_l1i_cache(void)
{
	unsigned long flags;

        local_irq_save(flags);
        sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_L1CACHE_I_PREFETCH, 0, 0, 0, 0, 0, 0);
        local_irq_restore(flags);
}

void sbi_enable_l1d_cache(void)
{
	unsigned long flags;

        local_irq_save(flags);
        sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_L1CACHE_D_PREFETCH, 1, 0, 0, 0, 0, 0);
        local_irq_restore(flags);
}

void sbi_disable_l1d_cache(void)
{
        unsigned long flags;

        local_irq_save(flags);
        sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_L1CACHE_D_PREFETCH, 0, 0, 0, 0, 0, 0);
        local_irq_restore(flags);
}
/* L1 Cache */
long cpu_l1c_status(void)
{
	struct sbiret ret;
	ret = sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_GET_MCACHE_CTL_STATUS, 0, 0, 0, 0, 0, 0);
	return ret.value;
}

void cpu_icache_enable(void *info)
{
	unsigned long flags;

	local_irq_save(flags);
	sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_ICACHE_OP, 1, 0, 0, 0, 0, 0);
	local_irq_restore(flags);
}

void cpu_icache_disable(void *info)
{
	unsigned long flags;

	local_irq_save(flags);
	sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_ICACHE_OP, 0, 0, 0, 0, 0, 0);
	local_irq_restore(flags);
}

void cpu_dcache_enable(void *info)
{
	unsigned long flags;

	local_irq_save(flags);
	sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_DCACHE_OP, 1, 0, 0, 0, 0, 0);
	local_irq_restore(flags);
}

void cpu_dcache_disable(void *info)
{
	unsigned long flags;

	local_irq_save(flags);
	sbi_ecall(SBI_EXT_ANDES, SBI_EXT_ANDES_DCACHE_OP, 0, 0, 0, 0, 0, 0);
	local_irq_restore(flags);
}

#ifndef CONFIG_PICOCOM_PC805
/* L2 Cache */
uint32_t cpu_l2c_ctl_status(void)
{
	return readl((void*)(l2c_base + L2C_REG_CTL_OFFSET));
}

void cpu_l2c_disable(void)
{
#ifdef CONFIG_SMP
	int mhartid = get_cpu();
#else
	int mhartid = 0;
#endif
	unsigned int val;
	unsigned long flags;

	/*No l2 cache */
	if(!l2c_base)
		return;

	/*l2 cache has disabled*/
	if(!(cpu_l2c_ctl_status() & L2_CACHE_CTL_mskCEN))
		return;

	local_irq_save(flags);

	/*Disable L2 cache*/
	val = readl((void*)(l2c_base + L2C_REG_CTL_OFFSET));
	val &= (~L2_CACHE_CTL_mskCEN);

	writel(val, (void*)(l2c_base + L2C_REG_CTL_OFFSET));
	while ((cpu_l2c_get_cctl_status() & CCTL_L2_STATUS_CN_MASK(mhartid))
	       != CCTL_L2_STATUS_IDLE);

	/*L2 write-back and invalidate all*/
	writel(CCTL_L2_WBINVAL_ALL, (void*)(l2c_base + L2C_REG_CN_CMD_OFFSET(mhartid)));
	while ((cpu_l2c_get_cctl_status() & CCTL_L2_STATUS_CN_MASK(mhartid))
	       != CCTL_L2_STATUS_IDLE);

	local_irq_restore(flags);
    #ifdef CONFIG_SMP
    put_cpu();
    #endif
}

#ifndef CONFIG_SMP
void cpu_l2c_inval_range(unsigned long pa, unsigned long size)
{
	unsigned long line_size = get_cache_line_size();
    unsigned long start = pa, end = pa + size;
    unsigned long align_start, align_end;

    align_start = start & ~(line_size - 1);
    align_end  = (end + line_size - 1) & ~(line_size - 1);

    while(align_end > align_start){
        writel(align_start, (void*)(l2c_base + L2C_REG_C0_ACC_OFFSET));
        writel(CCTL_L2_PA_INVAL, (void*)(l2c_base + L2C_REG_C0_CMD_OFFSET));
        while ((cpu_l2c_get_cctl_status() & CCTL_L2_STATUS_C0_MASK)
                != CCTL_L2_STATUS_IDLE);
        align_start += line_size;
	}
}
EXPORT_SYMBOL(cpu_l2c_inval_range);

void cpu_l2c_wb_range(unsigned long pa, unsigned long size)
{
    unsigned long line_size = get_cache_line_size();
    unsigned long start = pa, end = pa + size;
    unsigned long align_start, align_end;

    align_start = start & ~(line_size - 1);
    align_end  = (end + line_size - 1) & ~(line_size - 1);

    while(align_end > align_start){
        writel(align_start, (void*)(l2c_base + L2C_REG_C0_ACC_OFFSET));
        writel(CCTL_L2_PA_WB, (void*)(l2c_base + L2C_REG_C0_CMD_OFFSET));
        while ((cpu_l2c_get_cctl_status() & CCTL_L2_STATUS_C0_MASK)
                != CCTL_L2_STATUS_IDLE);
        align_start += line_size;
    }
}
EXPORT_SYMBOL(cpu_l2c_wb_range);
#else
void cpu_l2c_inval_range(unsigned long pa, unsigned long size)
{
    int mhartid = get_cpu();
    unsigned long line_size = get_cache_line_size();
    unsigned long start = pa, end = pa + size;
    unsigned long align_start, align_end;

    align_start = start & ~(line_size - 1);
    align_end  = (end + line_size - 1) & ~(line_size - 1);

    while(align_end > align_start){
        writel(align_start, (void*)(l2c_base + L2C_REG_CN_ACC_OFFSET(mhartid)));
        writel(CCTL_L2_PA_INVAL, (void*)(l2c_base + L2C_REG_CN_CMD_OFFSET(mhartid)));
        while ((cpu_l2c_get_cctl_status() & CCTL_L2_STATUS_CN_MASK(mhartid))
                != CCTL_L2_STATUS_IDLE);
        align_start += line_size;
    }
    put_cpu();
}
EXPORT_SYMBOL(cpu_l2c_inval_range);

void cpu_l2c_wb_range(unsigned long pa, unsigned long size)
{
    int mhartid = get_cpu();
    unsigned long line_size = get_cache_line_size();
    unsigned long start = pa, end = pa + size;
    unsigned long align_start, align_end;

    align_start = start & ~(line_size - 1);
    align_end  = (end + line_size - 1) & ~(line_size - 1);

    while(align_end > align_start){
        writel(align_start, (void*)(l2c_base + L2C_REG_CN_ACC_OFFSET(mhartid)));
        writel(CCTL_L2_PA_WB, (void*)(l2c_base + L2C_REG_CN_CMD_OFFSET(mhartid)));
        while ((cpu_l2c_get_cctl_status() & CCTL_L2_STATUS_CN_MASK(mhartid))
                != CCTL_L2_STATUS_IDLE);
        align_start += line_size;
    }
    put_cpu();
}
EXPORT_SYMBOL(cpu_l2c_wb_range);
#endif

#ifdef CONFIG_PERF_EVENTS
int cpu_l2c_get_counter_idx(struct l2c_hw_events *l2c)
{
	int idx;

	idx = find_next_zero_bit(l2c->used_mask, L2C_MAX_COUNTERS - 1, 0);
	return idx;
}

void l2c_write_counter(int idx, u64 value)
{
	u32 vall = value;
	u32 valh = value >> 32;

	writel(vall, (void*)(l2c_base + L2C_REG_CN_HPM_OFFSET(idx)));
	writel(valh, (void*)(l2c_base + L2C_REG_CN_HPM_OFFSET(idx) + 0x4));
}

u64 l2c_read_counter(int idx)
{
	u32 vall = readl((void*)(l2c_base + L2C_REG_CN_HPM_OFFSET(idx)));
	u32 valh = readl((void*)(l2c_base + L2C_REG_CN_HPM_OFFSET(idx) + 0x4));
	u64 val = ((u64)valh << 32) | vall;

	return val;
}

void l2c_pmu_disable_counter(int idx)
{
	int n = idx / SEL_PER_CTL;
	u32 vall = readl((void*)(l2c_base + L2C_HPM_CN_CTL_OFFSET(n)));
	u32 valh = readl((void*)(l2c_base + L2C_HPM_CN_CTL_OFFSET(n) + 0x4));
	u64 val = ((u64)valh << 32) | vall;

	val |= (EVSEL_MASK << SEL_OFF(idx));
	vall = val;
	valh = val >> 32;
	writel(vall, (void*)(l2c_base + L2C_HPM_CN_CTL_OFFSET(n)));
	writel(valh, (void*)(l2c_base + L2C_HPM_CN_CTL_OFFSET(n) + 0x4));
}

#ifndef CONFIG_SMP
void l2c_pmu_event_enable(u64 config, int idx)
{
	int n = idx / SEL_PER_CTL;
	u32 vall = readl((void*)(l2c_base + L2C_HPM_CN_CTL_OFFSET(n)));
	u32 valh = readl((void*)(l2c_base + L2C_HPM_CN_CTL_OFFSET(n) + 0x4));
	u64 val = ((u64)valh << 32) | vall;

	val = val & ~(EVSEL_MASK << SEL_OFF(idx));
	val = val | (config << SEL_OFF(idx));
	vall = val;
	valh = val >> 32;
	writel(vall, (void*)(l2c_base + L2C_HPM_CN_CTL_OFFSET(n)));
	writel(valh, (void*)(l2c_base + L2C_HPM_CN_CTL_OFFSET(n) + 0x4));
}
#else
void l2c_pmu_event_enable(u64 config, int idx)
{
	int n = idx / SEL_PER_CTL;
	int mhartid = get_cpu();
	u32 vall = readl((void*)(l2c_base + L2C_HPM_CN_CTL_OFFSET(n)));
	u32 valh = readl((void*)(l2c_base + L2C_HPM_CN_CTL_OFFSET(n) + 0x4));
	u64 val = ((u64)valh << 32) | vall;

	if (config <= (CN_RECV_SNOOP_DATA(NR_CPUS - 1) & EVSEL_MASK))
		config = config + mhartid * L2C_REG_PER_CORE_OFFSET;

	val = val & ~(EVSEL_MASK << SEL_OFF(idx));
	val = val | (config << SEL_OFF(idx));
	vall = val;
	valh = val >> 32;
	writel(vall, (void*)(l2c_base + L2C_HPM_CN_CTL_OFFSET(n)));
	writel(valh, (void*)(l2c_base + L2C_HPM_CN_CTL_OFFSET(n) + 0x4));
    put_cpu();
}
#endif
#endif

int __init l2c_init(void)
{
	struct device_node *node ;

	node = of_find_compatible_node(NULL, NULL, "cache");
	l2c_base = of_iomap(node, 0);

	return 0;
}
arch_initcall(l2c_init)
#endif /* !CONFIG_PICOCOM_PC805 */

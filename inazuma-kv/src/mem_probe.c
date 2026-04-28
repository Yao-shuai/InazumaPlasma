/**
 * @file        mem_probe.c
 * @brief       InazumaKV 全局内存遥测与探针模块 (Always-On 模式)
 * @author      Nexus_Yao
 * @date        2026-XX-XX
 * @version     1.0.0
 * @note        采用 C11 标准的 stdatomic 实现无锁内存监控。
 * 支持通过 SIGUSR1 信号触发终端大屏打印，并提供兼容 Redis INFO 协议的
 * RESP 报文输出，便于无缝接入 Prometheus / redis_exporter 等标准云原生监控生态。
 */

#include "mem_probe.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>

/* ==========================================================================
 * 全局原子遥测指标 (Global Atomic Telemetry Metrics)
 * ========================================================================== */

/** @brief 探针全局开关状态 (默认持续开启) */
static atomic_bool g_probe_enabled = ATOMIC_VAR_INIT(true); 

/** @brief 操作系统层面内存分配 (malloc/calloc) 的总调用次数 */
static atomic_long g_os_alloc_count = ATOMIC_VAR_INIT(0);

/** @brief 操作系统层面内存释放 (free) 的总调用次数 */
static atomic_long g_os_free_count  = ATOMIC_VAR_INIT(0);

/** @brief 当前进程生命周期内逻辑分配的活跃内存总量 (Bytes) */
static atomic_long g_os_active_bytes = ATOMIC_VAR_INIT(0);

/** @brief 当前进程生命周期内逻辑分配内存的历史最高峰值 (Bytes) */
static atomic_long g_os_peak_bytes   = ATOMIC_VAR_INIT(0); 


/* ==========================================================================
 * 内部辅助探测机制 (Internal Probing Mechanisms)
 * ========================================================================== */

/**
 * @brief       读取 Linux 内核视角的真实物理内存驻留大小 (RSS)
 * @return      long 物理常驻内存大小 (Bytes)
 * @note        通过读取 VFS 虚拟文件系统 /proc/self/statm 的第二列获取页面数，
 * 乘以 sysconf 获取的系统页大小 (通常为 4KB)，计算出精确的 RSS 消耗。
 */
static long get_kernel_rss_bytes() {
    long rss = 0;
    FILE *fp = fopen("/proc/self/statm", "r");
    if (fp) {
        long dummy;
        /* statm 第二列为 resident set size */
        if (fscanf(fp, "%ld %ld", &dummy, &rss) == 2) {
            rss *= sysconf(_SC_PAGESIZE); 
        }
        fclose(fp);
    }
    return rss;
}

/**
 * @brief       SIGUSR1 信号拦截处理例程：终端输出遥测视图
 * @param[in]   sig 捕获到的信号量编号
 * @warning     [架构级提示] POSIX 规范指出 printf 族函数并非 Async-Signal-Safe。
 * 在极端高并发导致的中断截断场景下，使用 printf 可能引发 stdout 锁的死锁现象。
 * 工业级方案通常使用 write(STDOUT_FILENO, ...) 替代，此处按原逻辑保留。
 */
static void handle_sigusr1(int sig) {
    long allocs = atomic_load_explicit(&g_os_alloc_count, memory_order_relaxed);
    long frees  = atomic_load_explicit(&g_os_free_count, memory_order_relaxed);
    long bytes  = atomic_load_explicit(&g_os_active_bytes, memory_order_relaxed);
    long peak   = atomic_load_explicit(&g_os_peak_bytes, memory_order_relaxed);
    long rss_bytes = get_kernel_rss_bytes();
    
    double frag_ratio = bytes > 0 ? (double)rss_bytes / bytes : 1.0;

    printf("\n==================================================\n");
    printf("          InazumaKV 内存遥测(运行中...)\n");
    printf("==================================================\n");
    printf(" [1. 操作系统内核内存 - 宏观视图]\n");
    printf("  > 系统内存分配次数 (Malloc)   : %ld 次\n", allocs);
    printf("  > 系统内存释放次数 (Free)     : %ld 次\n", frees);
    printf("  > 逻辑预分配总内存 (Logical)  : %ld Bytes (%.2f MB)\n", bytes, (double)bytes/(1024*1024));
    printf("  > 逻辑内存历史峰值 (Peak)     : %ld Bytes (%.2f MB)\n", peak, (double)peak/(1024*1024));
    printf("  > 物理常驻内存消耗 (RSS)      : %ld Bytes (%.2f MB)\n", rss_bytes, (double)rss_bytes/(1024*1024));
    printf("  > 物理内存驻留率 (RSS Ratio)  : %.2f\n", frag_ratio);
    printf("==================================================\n\n");
}


/* ==========================================================================
 * 对外暴露接口 (Public Probing APIs)
 * ========================================================================== */

/**
 * @brief       生成兼容 redis_exporter 格式的 INFO MEMORY 协议报文
 * @param[out]  buffer  承接 RESP 字符串的预分配缓冲区
 * @param[in]   max_len 缓冲区的最大安全尺寸
 * @return      int     实际写入缓冲区的字节总数
 * @note        此函数徒手拼装的文本格式严格遵循 Redis INFO 命令的标准段落格式，
 * 使得第三方监控体系可以零代码修改直接挂载并采集本引擎的指标数据。
 */
int mem_probe_generate_info_memory(char *buffer, size_t max_len) {
    long bytes  = atomic_load_explicit(&g_os_active_bytes, memory_order_relaxed);
    long peak   = atomic_load_explicit(&g_os_peak_bytes, memory_order_relaxed);
    long allocs = atomic_load_explicit(&g_os_alloc_count, memory_order_relaxed);
    long rss_bytes = get_kernel_rss_bytes();
    
    double frag_ratio = bytes > 0 ? (double)rss_bytes / bytes : 1.0;

    char temp[2048];
    int len = snprintf(temp, sizeof(temp),
        "# Memory\r\n"
        "used_memory:%ld\r\n"
        "used_memory_human:%.2fM\r\n"
        "used_memory_rss:%ld\r\n"
        "used_memory_rss_human:%.2fM\r\n"
        "used_memory_peak:%ld\r\n"
        "used_memory_peak_human:%.2fM\r\n"
        "mem_fragmentation_ratio:%.2f\r\n"
        "mem_allocator:jemalloc\r\n"
        "os_malloc_calls:%ld\r\n",
        bytes, (double)bytes/(1024*1024),
        rss_bytes, (double)rss_bytes/(1024*1024),
        peak, (double)peak/(1024*1024),
        frag_ratio,
        allocs
    );

    return snprintf(buffer, max_len, "$%d\r\n%s\r\n", len, temp);
}

/**
 * @brief       初始化内存探针模块
 * @param[in]   enable 外部传入的开关标志位 (被模块内部强制覆盖为开启)
 * @note        执行探针状态的原子写入，并挂载 SIGUSR1 的软中断陷阱例程。
 */
void mem_probe_init(int enable) {
    /* 忽略外部传入的 enable，强制设为 true，并绑定信号 */
    atomic_store_explicit(&g_probe_enabled, true, memory_order_release);
    signal(SIGUSR1, handle_sigusr1); 
    
    printf("[MemProbe] 企业级内置遥测探针已永久激活...(物理查岗: sudo kill -SIGUSR1 %d)\n", getpid());
}

/**
 * @brief       探测内存分配事件，驱动遥测指标累加
 * @param[in]   size 单次分配动作申请的字节数
 * @note        利用 Compare-And-Swap (CAS) 乐观锁更新峰值指标，
 * 避免了互斥锁 (Mutex) 在高并发内存分配阶段造成的线程上下文切换开销。
 */
void mem_probe_alloc(size_t size) {
    atomic_fetch_add_explicit(&g_os_alloc_count, 1, memory_order_relaxed);
    
    long current = atomic_fetch_add_explicit(&g_os_active_bytes, size, memory_order_relaxed) + size;
    long peak = atomic_load_explicit(&g_os_peak_bytes, memory_order_relaxed);
    
    /* 无锁自旋：确保并发更新峰值时的数据一致性 */
    while (current > peak && 
           !atomic_compare_exchange_weak_explicit(&g_os_peak_bytes, &peak, current, 
                                                  memory_order_release, memory_order_relaxed));
}

/**
 * @brief       探测内存释放事件，驱动遥测指标递减
 * @param[in]   size 单次释放动作回收的字节数
 */
void mem_probe_free(size_t size) {
    atomic_fetch_add_explicit(&g_os_free_count, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g_os_active_bytes, size, memory_order_relaxed);
}
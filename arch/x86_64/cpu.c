#include <anix/cpu.h>
#include <anix/string.h>
#define IA32_EFER 0xc0000080u
#define EFER_NXE (1ULL << 11)
#define CR0_MP (1ULL << 1)
#define CR0_EM (1ULL << 2)
#define CR0_TS (1ULL << 3)
#define CR0_NE (1ULL << 5)
#define CR0_WP (1ULL << 16)
#define CR4_OSFXSR (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_UMIP (1ULL << 11)
#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)
static bool smap_active;
static uint8_t initial_fpu_state[512] __attribute__((aligned(16)));
static bool fpu_initialized;
static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(subleaf));
}
static uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}
static void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory");
}
void cpu_detect_features(struct cpu_features *features) {
    if (!features) return;
    *features = (struct cpu_features){0};
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 1u) {
        cpuid(1u, 0, &a, &b, &c, &d);
        features->fpu = (d & (1u << 0)) != 0;
        features->fxsave = (d & (1u << 24)) != 0;
        features->sse2 = (d & (1u << 26)) != 0;
    }
    cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000001u) {
        cpuid(0x80000001u, 0, &a, &b, &c, &d);
        features->syscall = (d & (1u << 11)) != 0;
        features->nx = (d & (1u << 20)) != 0;
        features->page_1g = (d & (1u << 26)) != 0;
    }
    cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 7u) {
        cpuid(7u, 0, &a, &b, &c, &d);
        features->smep = (b & (1u << 7)) != 0;
        features->smap = (b & (1u << 20)) != 0;
        features->umip = (c & (1u << 2)) != 0;
    }
}
void cpu_enable_hardening(const struct cpu_features *features) {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_WP;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (features && features->umip) cr4 |= CR4_UMIP;
    if (features && features->smep) cr4 |= CR4_SMEP;
    if (features && features->smap) cr4 |= CR4_SMAP;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    smap_active = features && features->smap;
}
bool cpu_smap_active(void) { return smap_active; }
bool cpu_enable_fpu(const struct cpu_features *features) {
    if (!features || !features->fpu || !features->fxsave || !features->sse2) return false;
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 | CR0_MP | CR0_NE) & ~(CR0_EM | CR0_TS);
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    uint32_t mxcsr = 0x1f80u;
    __asm__ volatile(
        "fninit\n\t"
        "ldmxcsr %0\n\t"
        "pxor %%xmm0, %%xmm0\n\t"
        "pxor %%xmm1, %%xmm1\n\t"
        "pxor %%xmm2, %%xmm2\n\t"
        "pxor %%xmm3, %%xmm3\n\t"
        "pxor %%xmm4, %%xmm4\n\t"
        "pxor %%xmm5, %%xmm5\n\t"
        "pxor %%xmm6, %%xmm6\n\t"
        "pxor %%xmm7, %%xmm7\n\t"
        "pxor %%xmm8, %%xmm8\n\t"
        "pxor %%xmm9, %%xmm9\n\t"
        "pxor %%xmm10, %%xmm10\n\t"
        "pxor %%xmm11, %%xmm11\n\t"
        "pxor %%xmm12, %%xmm12\n\t"
        "pxor %%xmm13, %%xmm13\n\t"
        "pxor %%xmm14, %%xmm14\n\t"
        "pxor %%xmm15, %%xmm15\n\t"
        "fxsave64 %1"
        : : "m"(mxcsr), "m"(initial_fpu_state) : "memory");
    fpu_initialized = true;
    return true;
}
void cpu_fpu_state_init(void *state) {
    if (!state || !fpu_initialized) return;
    kmemcpy(state, initial_fpu_state, sizeof(initial_fpu_state));
}
bool cpu_enable_nx(void) {
    struct cpu_features features;
    cpu_detect_features(&features);
    if (!features.nx) return false;
    uint64_t efer = rdmsr(IA32_EFER);
    if (!(efer & EFER_NXE)) wrmsr(IA32_EFER, efer | EFER_NXE);
    return (rdmsr(IA32_EFER) & EFER_NXE) != 0;
}

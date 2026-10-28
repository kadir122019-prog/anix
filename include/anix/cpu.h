#ifndef ANIX_CPU_H
#define ANIX_CPU_H
#include <anix/types.h>
struct cpu_features {
    bool fpu;
    bool fxsave;
    bool sse2;
    bool nx;
    bool page_1g;
    bool syscall;
    bool smep;
    bool smap;
    bool umip;
};
void cpu_detect_features(struct cpu_features *features);
bool cpu_enable_nx(void);
void cpu_enable_hardening(const struct cpu_features *features);
bool cpu_enable_fpu(const struct cpu_features *features);
void cpu_fpu_state_init(void *state);
bool cpu_smap_active(void);
#endif

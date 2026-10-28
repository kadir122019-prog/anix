#ifndef ANIX_KERNEL_IMAGE_H
#define ANIX_KERNEL_IMAGE_H
#include <anix/types.h>
#include <anix/vmm.h>
struct kernel_section { const char *name; uint64_t start, end; uint64_t required_flags, forbidden_flags; };
size_t kernel_image_sections(struct kernel_section *sections, size_t capacity);
bool kernel_image_validate_mappings(uint64_t active_root_phys);
bool kernel_image_build_candidate(const struct vmm_space *source, struct vmm_space *candidate, bool nx_enabled);
bool kernel_candidate_clone_live_ranges(const struct vmm_space *source, struct vmm_space *candidate);
bool kernel_candidate_validate_live_ranges(const struct vmm_space *candidate);
bool kernel_candidate_clone_hhdm(const struct vmm_space *source, struct vmm_space *candidate);
bool kernel_candidate_validate_hhdm(const struct vmm_space *candidate);
bool kernel_candidate_clone_boot_resources(const struct vmm_space *source, struct vmm_space *candidate);
bool kernel_candidate_validate_boot_resources(const struct vmm_space *candidate);
#endif

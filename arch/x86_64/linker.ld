OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kmain)
PHDRS { requests PT_LOAD FLAGS(4); text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0xffffffff80000000;
  .limine_requests : { KEEP(*(.limine_requests_start)) KEEP(*(.limine_requests)) KEEP(*(.limine_requests_end)) } :requests
  . = ALIGN(CONSTANT(MAXPAGESIZE));
  __text_start = .;
  .text : { *(.text .text.*) } :text
  __text_end = .;
  . = ALIGN(CONSTANT(MAXPAGESIZE));
  __rodata_start = .;
  .rodata : { *(.rodata .rodata.*) } :rodata
  __rodata_end = .;
  . = ALIGN(CONSTANT(MAXPAGESIZE));
  __data_start = .;
  .data : { *(.data .data.*) } :data
  .bss : { *(COMMON) *(.bss .bss.*) } :data
  __kernel_end = .;
  /DISCARD/ : { *(.eh_frame*) *(.note*) }
}

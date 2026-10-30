ENTRY(probe_entry)
SECTIONS
{
  . = 0x101c7180;
  .text : { KEEP(*(.entry)) *(.text*) *(.rodata*) }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment*) }
}

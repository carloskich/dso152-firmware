/* DSO152 application image.
 * FLASH: 0x08003C00 .. 0x0800F7FF (47 KB). Below is the FNIRSI bootloader,
 * above are our settings page (0x0800F800) and the FNIRSI calibration page
 * (0x0800FC00). The linker refuses an image that does not fit, so nothing
 * we build can ever be placed over the bootloader or the calibration.
 * RAM: 20 KB.
 */
ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size  = 0x0;
_Min_Stack_Size = 0x600;

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08003C00, LENGTH = 0x4400
  FLASH2 (rx) : ORIGIN = 0x08009A00, LENGTH = 0x5E00
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 20K
}

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  /* Above the bootloader's font area: the main program and the USB driver
     (code and constants), so the area below keeps room to spare. Listed
     before .text so that these objects are taken here first. */
  .text2 :
  {
    . = ALIGN(4);
    *main.o(.text .text* .rodata .rodata*)
    *usb.o(.text .text* .rodata .rodata*)
    . = ALIGN(4);
  } >FLASH2

  .text :
  {
    . = ALIGN(4);
    *(.text) *(.text*) *(.glue_7) *(.glue_7t) *(.eh_frame)
    KEEP(*(.init)) KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata) *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  /* 0x08008000..0x080099FF is left for the stock bytes: the FNIRSI
     bootloader reads its font there. Fonts go above it. */
  .fonts :
  {
    . = ALIGN(4);
    KEEP(*(.fonts*))
    . = ALIGN(4);
  } >FLASH2

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH

  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } >FLASH
  .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } >FLASH
  .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data) *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss) *(.bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  .ARM.attributes 0 : { *(.ARM.attributes) }
}

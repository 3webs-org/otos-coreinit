/* The separately linked stage-2 Mach-O bytes. Nothing here links its
 * symbols into coreinit: the stage loader reads these bytes and places a
 * fresh image inside memory claimed from the memory-manager object. */
.section __DATA,__const
.p2align 4
.globl _securinit_image_start
_securinit_image_start:
.incbin SECURINIT_MACHO_PATH
.globl _securinit_image_end
_securinit_image_end:

' W200: value-will-be-truncated. This warning lives in zxbasm (the assembler),
' not the zxbc codegen modules, so it can only surface on the end-to-end
' binary stage (zxbc internally assembles the inline ASM). `ld a, 300` loads
' an 8-bit register with a >8-bit value -> warning_value_will_be_truncated.
Asm
  ld a, 300
End Asm
Print 1

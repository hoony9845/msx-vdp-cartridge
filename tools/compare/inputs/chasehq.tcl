# Scripted input for Chase H.Q. V9990 (DiHalt Studio, ASCII16 MegaROM):
# start the game, skip the briefing, then accelerate and steer.
# Use with record.sh:  V9990_EXTRA_SCRIPT=tools/compare/inputs/chasehq.tcl
#
# MSX keyboard matrix row 8: bit0 space, bit4 left, bit5 up, bit7 right.

proc key_tap {mask {dur 0.15}} {
    keymatrixdown 8 $mask
    after time $dur [list keymatrixup 8 $mask]
}

proc steer {n} {
    # alternate left / right while the accelerator stays pressed
    set mask [expr {$n % 2 ? 0x10 : 0x80}]
    keymatrixdown 8 $mask
    after time 0.6 [list keymatrixup 8 $mask]
    after time 1.8 [list steer [expr {$n + 1}]]
}

foreach t {32 35 38 41 44} { after time $t {key_tap 0x01} }
after time 46 { keymatrixdown 8 0x20; steer 0 }

# Run internal-V9968 software (ports 0x98-0x9C, geo3d 0x9D/0x9F) and note
# every instruction that accesses those ports: its PC, the port, read or
# write, and the memory bytes around it (to find it in the ROM image).
#
# Environment:
#   PORT_TRACE_OUT     output file
#   PORT_TRACE_FRAMES  stop after this many frames (default 18000 = 5 min);
#                      0 = play it yourself: real speed, no limit, the file
#                      is rewritten every 2 seconds while new sites turn up,
#                      so closing the window loses nothing

set ::pt_out    $::env(PORT_TRACE_OUT)
set ::pt_limit  [expr {[info exists ::env(PORT_TRACE_FRAMES)] ? $::env(PORT_TRACE_FRAMES) : 18000}]
set ::pt_frame  0
set ::pt_seen   [dict create]
set ::pt_saved  0

proc pt_note {kind} {
    set pc [reg pc]
    set port [expr {$::wp_last_address & 0xFF}]
    set key "$pc $port $kind"
    if {[dict exists $::pt_seen $key]} return
    # 40 bytes before the PC (the instruction and what set its registers)
    # and 8 after; the PC is past the I/O instruction here
    set bytes {}
    for {set a [expr {$pc - 40}]} {$a < $pc + 8} {incr a} {
        lappend bytes [format %02X [debug read memory [expr {$a & 0xFFFF}]]]
    }
    dict set ::pt_seen $key [list [format %04X $pc] [format %02X $port] $kind [reg bc] [join $bytes ""]]
}

proc pt_save {} {
    if {[catch {
        set fd [open $::pt_out.tmp w]
        dict for {k v} $::pt_seen { puts $fd [join $v " "] }
        close $fd
        file rename -force $::pt_out.tmp $::pt_out
        set ::pt_saved [dict size $::pt_seen]
    } err]} { puts stderr "port trace: ERROR $err" }
}

proc pt_frame_tick {} {
    incr ::pt_frame
    if {$::pt_limit == 0} {
        if {$::pt_frame % 120 == 0 && [dict size $::pt_seen] != $::pt_saved} {
            set new [expr {[dict size $::pt_seen] - $::pt_saved}]
            pt_save
            puts stderr "port trace: $new new sites, [dict size $::pt_seen] in all"
            catch {message "port trace: [dict size $::pt_seen] sites"}
        }
    } elseif {$::pt_frame >= $::pt_limit} {
        pt_save
        puts stderr "port trace: [dict size $::pt_seen] sites after $::pt_frame frames -> $::pt_out"
        exit
    }
    after frame pt_frame_tick
}

debug set_watchpoint write_io {0x98 0x9F} {} {pt_note w}
debug set_watchpoint read_io  {0x98 0x9F} {} {pt_note r}
after frame pt_frame_tick
if {$::pt_limit == 0} {
    set ::throttle on
    after time 1 { catch {set ::videosource V9968} }
} else {
    set ::throttle off
}

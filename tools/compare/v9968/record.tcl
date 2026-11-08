# Record every Z80 access to the V9968 cartridge ports (0x88-0x8F: V9968
# 0x88-0x8C, geo3d 0x8D/0x8F) of running MSX software as a trace (format:
# see host/replay9968.c), with a "frame" line per video frame and a
# screenshot of the cartridge output every N frames. V9968 counterpart of
# ../record.tcl.
#
# Environment (set by record.sh):
#   V9968_RECORD        output directory (trace.txt + f<frame>.png)
#   V9968_SHOT_EVERY    screenshot interval in frames (default 60)
#   V9968_FRAMES        stop after this many frames (default: until quit)
#   V9968_EXTRA_SCRIPT  optional Tcl script (inputs)

set ::rec_dir   $::env(V9968_RECORD)
set ::rec_every [expr {[info exists ::env(V9968_SHOT_EVERY)] ? $::env(V9968_SHOT_EVERY) : 60}]
set ::rec_limit [expr {[info exists ::env(V9968_FRAMES)] ? $::env(V9968_FRAMES) : 0}]
set ::rec_frame 0
file mkdir $::rec_dir
set ::rec_fd [open [file join $::rec_dir trace.txt] w]
fconfigure $::rec_fd -buffering full

proc rec_write {} {
    puts $::rec_fd [format "w 0x%02X 0x%02X" [expr {$::wp_last_address & 0xFF}] $::wp_last_value]
}

proc rec_read {} {
    puts $::rec_fd [format "r 0x%02X" [expr {$::wp_last_address & 0xFF}]]
}

proc rec_next_frame {} {
    incr ::rec_frame
    puts $::rec_fd "frame"
    if {$::rec_frame % $::rec_every == 0} {
        set name [format "f%06d" $::rec_frame]
        puts $::rec_fd "shot $name"
        screenshot [file join $::rec_dir "$name.png"]
    }
    flush $::rec_fd
    if {$::rec_limit > 0 && $::rec_frame >= $::rec_limit} {
        close $::rec_fd
        puts stderr "recorded $::rec_frame frames into $::rec_dir"
        exit
    }
    after frame rec_next_frame
}

debug set_watchpoint write_io {0x88 0x8F} {} rec_write
debug set_watchpoint read_io  {0x88 0x8F} {} rec_read
after frame rec_next_frame
# the cartridge's video source exists once the machine runs
after time 1 { if {[catch {set ::videosource V9968} err]} { puts stderr "ERROR: $err" } }
puts stderr "recording V9968 port accesses into $::rec_dir"

if {[info exists ::env(V9968_EXTRA_SCRIPT)]} {
    source $::env(V9968_EXTRA_SCRIPT)
}

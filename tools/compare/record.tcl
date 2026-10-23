# Record every Z80 access to the V9990 ports (0x60-0x6F) of running MSX
# software as a trace (format: see host/replay.c), with a "frame" line per
# video frame and a screenshot every N frames.
#
# Environment (set by record.sh):
#   V9990_RECORD        output directory (trace.txt + f<frame>.png)
#   V9990_SHOT_EVERY    screenshot interval in frames (default 60)
#   V9990_FRAMES        stop after this many frames (default: until quit)
#
# Screenshots are taken at frame boundaries; the replay renders the state at
# the same point, so pictures that change registers mid-frame (raster
# effects) can differ by design.

set ::rec_dir   $::env(V9990_RECORD)
set ::rec_every [expr {[info exists ::env(V9990_SHOT_EVERY)] ? $::env(V9990_SHOT_EVERY) : 60}]
set ::rec_limit [expr {[info exists ::env(V9990_FRAMES)] ? $::env(V9990_FRAMES) : 0}]
set ::rec_frame 0
file mkdir $::rec_dir
set ::rec_fd [open [file join $::rec_dir trace.txt] w]
fconfigure $::rec_fd -buffering full

proc rec_write {} {
    # OUT (C),A puts B on A15-A8: keep the low byte only
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

set ::videosource GFX9000
debug set_watchpoint write_io {0x60 0x6F} {} rec_write
debug set_watchpoint read_io  {0x60 0x6F} {} rec_read
after frame rec_next_frame
puts stderr "recording V9990 port accesses into $::rec_dir"

# optional extra script (openMSX only takes one -script), e.g. the self-test loader
if {[info exists ::env(V9990_EXTRA_SCRIPT)]} {
    source $::env(V9990_EXTRA_SCRIPT)
}

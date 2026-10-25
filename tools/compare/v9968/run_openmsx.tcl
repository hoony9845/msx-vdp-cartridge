# Execute a V9968 port trace (format: see host/replay9968.c) on the openMSX
# V9968 + geo3d fork and save screenshots for each "shot" line.
#
# Started by run_openmsx.sh with the environment variables
#   V9968_TRACE  trace file
#   V9968_OUT    output directory for <name>.png
#
# Port writes use the "ioports" debuggable (a real I/O write); "r" lines are
# executed as debug reads, which do not have read side effects in openMSX.

set ::trace_lines [split [read [open $::env(V9968_TRACE)]] "\n"]
set ::trace_idx 0
set ::out_dir $::env(V9968_OUT)
set ::trace_writes 0

proc trace_shot {name} {
    if {[catch {screenshot [file join $::out_dir "$name.png"]} err]} {
        puts stderr "screenshot $name failed: $err"
    } else {
        puts stderr "shot $name"
    }
    trace_step
}

proc trace_step {} {
    if {[catch {trace_run} err]} {
        puts stderr "ERROR at line $::trace_idx: $err"
        exit 1
    }
}

# The fork shows CE neither in a port peek nor in "V9968 status regs", so a
# command is given a fixed time (more than any command of the scenarios
# takes); geo3d's RUN busy is readable (index port, no side effects).
proc geo3d_busy {} {
    expr {[debug read ioports 0x8D] & 1}
}

proc trace_wait_geo3d {} {
    if {[geo3d_busy]} {
        after time 0.01 trace_wait_geo3d
    } else {
        trace_step
    }
}

proc trace_run {} {
    set n [llength $::trace_lines]
    while {$::trace_idx < $n} {
        set line [string trim [lindex $::trace_lines $::trace_idx]]
        incr ::trace_idx
        set hash [string first "#" $line]
        if {$hash >= 0} { set line [string trim [string range $line 0 [expr {$hash - 1}]]] }
        if {$line eq ""} continue
        set cmd [lindex $line 0]
        switch -- $cmd {
            w {
                debug write ioports [expr {[lindex $line 1]}] [expr {[lindex $line 2]}]
                incr ::trace_writes
            }
            r {
                debug read ioports [expr {[lindex $line 1]}]
            }
            wait_ce {
                after time 0.15 trace_wait_geo3d
                return
            }
            frame {
                after frame trace_step
                return
            }
            shot {
                after frame [list after frame [list trace_shot [lindex $line 1]]]
                return
            }
            default {
                error "unknown trace command: $line"
            }
        }
    }
    puts stderr "trace done: $::trace_writes writes"
    exit
}

# the cartridge's video source exists once the machine runs
# the trace writes VRAM back to back through the debugger: no access slots
catch {set too_fast_vram_access ignore}
after time 2 {
    if {[catch {set ::videosource V9968} err]} {
        puts stderr "ERROR: $err"
        exit 1
    }
    trace_step
}

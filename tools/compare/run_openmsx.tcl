# Execute a V9990 port trace (format: see host/replay.c) on openMSX's GFX9000
# and save screenshots for each "shot" line.
#
# Started by run_openmsx.sh with the environment variables
#   V9990_TRACE  trace file
#   V9990_OUT    output directory for <name>.png
#
# Port writes use the "ioports" debuggable (a real I/O write); "r" lines are
# executed as debug reads, which do not have read side effects in openMSX.

set ::trace_lines [split [read [open $::env(V9990_TRACE)]] "\n"]
set ::trace_idx 0
set ::out_dir $::env(V9990_OUT)
set ::trace_writes 0

proc trace_dump {} {
    if {![info exists ::env(V9990_DEBUG)]} return
    set r [debug read_block "Sunrise GFX9000 regs" 0 32]
    binary scan $r cu* regs
    puts stderr "regs 0-31: $regs"
    foreach a {0x00000 0x40000 0x7C000 0x7E000 0x3FE00} {
        set m [debug read_block "Sunrise GFX9000 VRAM" $a 8]
        binary scan $m cu* b
        puts stderr "vram [format %05X $a]: $b"
    }
}

proc trace_shot {name} {
    trace_dump
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
                if {[debug read ioports 0x65] & 1} {
                    incr ::trace_idx -1
                    after time 0.0002 trace_step
                    return
                }
            }
            frame {
                after frame trace_step
                return
            }
            shot {
                # let the renderer show the current state, then capture
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

set ::videosource GFX9000
# give the machine (C-BIOS) time to boot before touching the V9990
after time 2 trace_step

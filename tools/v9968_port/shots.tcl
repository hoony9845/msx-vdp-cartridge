# Screenshots of the V9968 picture every SHOT_EVERY frames until SHOT_FRAMES.
set ::sh_dir $::env(SHOT_DIR)
set ::sh_every $::env(SHOT_EVERY)
set ::sh_limit $::env(SHOT_FRAMES)
set ::sh_n 0
file mkdir $::sh_dir
proc sh_tick {} {
    incr ::sh_n
    if {$::sh_n % $::sh_every == 0} { screenshot [file join $::sh_dir [format f%06d.png $::sh_n]] }
    if {$::sh_n >= $::sh_limit} { exit }
    after frame sh_tick
}
after frame sh_tick
after time 1 { catch {set ::videosource V9968} }
set ::throttle off

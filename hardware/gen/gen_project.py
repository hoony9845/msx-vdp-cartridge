"""
Write the KiCad project file with design rules (JLCPCB 2-layer friendly)
and net classes.

    python3 gen_project.py <project_dir>
"""
import json
import os
import sys

import design

out_dir = sys.argv[1]
os.makedirs(out_dir, exist_ok=True)


def netclass(name, track, clearance=0.2, via=0.6, drill=0.3, prio=0):
    return {
        "name": name, "clearance": clearance, "track_width": track,
        "via_diameter": via, "via_drill": drill,
        "microvia_diameter": 0.3, "microvia_drill": 0.1,
        "diff_pair_width": 0.2, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25,
        "wire_width": 6, "bus_width": 12, "line_style": 0,
        "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)",
        "priority": prio,
    }


pro = {
    "meta": {"filename": design.PROJECT + ".kicad_pro", "version": 1},
    "board": {
        "design_settings": {
            "defaults": {"board_outline_line_width": 0.1, "copper_line_width": 0.2,
                         "silk_line_width": 0.12, "silk_text_size_h": 1.0,
                         "silk_text_size_v": 1.0, "silk_text_thickness": 0.15},
            "rules": {
                "min_clearance": 0.15, "min_track_width": 0.15,
                "min_connection": 0.13,   # JLCPCB 2-layer minimum is 0.127mm
                "min_via_annular_width": 0.13,
                "min_via_diameter": 0.5, "min_through_hole_diameter": 0.3,
                "min_hole_clearance": 0.25, "min_hole_to_hole": 0.25,
                "min_copper_edge_clearance": 0.3, "min_silk_clearance": 0.0,
                "min_text_height": 0.8, "min_text_thickness": 0.12,
                "solder_mask_to_copper_clearance": 0.0,
                "min_microvia_diameter": 0.2, "min_microvia_drill": 0.1,
                "allow_blind_buried_vias": False, "allow_microvias": False,
            },
            "rule_severities": {
                # The Pico module and the HDMI shell deliberately overhang the board edge
                "courtyards_overlap": "error",
                "silk_edge_clearance": "ignore",
                "silk_over_copper": "warning",
                "silk_overlap": "ignore",
                "text_height": "warning",
                "lib_footprint_issues": "ignore",
                "lib_footprint_mismatch": "ignore",
            },
            "track_widths": [0.0, 0.25, 0.3, 0.5, 0.8],
            "via_dimensions": [{"diameter": 0.0, "drill": 0.0}, {"diameter": 0.6, "drill": 0.3}],
        },
    },
    "net_settings": {
        "meta": {"version": 3},
        "classes": [
            netclass("Default", 0.25, prio=2147483647),
            netclass("Power", 0.4, prio=0),
            netclass("DVI", 0.25, clearance=0.15, prio=1),
        ],
        "netclass_patterns": [
            {"netclass": "Power", "pattern": "GND"},
            {"netclass": "Power", "pattern": "+5V"},
            {"netclass": "Power", "pattern": "+3V3"},
            {"netclass": "Power", "pattern": "/VSYS"},
            {"netclass": "DVI", "pattern": "/DVI_*"},
            {"netclass": "DVI", "pattern": "/HDMI_*"},
        ],
    },
    "erc": {
        "rule_severities": {
            "lib_symbol_issues": "warning",
        },
    },
    "schematic": {"legacy_lib_dir": "", "legacy_lib_list": []},
    "sheets": [],
    "text_variables": {},
}

with open(os.path.join(out_dir, design.PROJECT + ".kicad_pro"), "w") as f:
    json.dump(pro, f, indent=2)

with open(os.path.join(out_dir, design.PROJECT + ".kicad_dru"), "w") as f:
    f.write("""(version 1)
# A single thermal spoke is acceptable on this board (hand-soldered parts).
(rule "thermal_spokes"
  (constraint min_resolved_spokes 1))
# The gold fingers run right up to the bottom board edge (bevelled at JLCPCB).
(rule "gold_fingers_to_edge"
  (condition "A.memberOfFootprint('J1')")
  (constraint edge_clearance (min 0)))
""")

with open(os.path.join(out_dir, "fp-lib-table"), "w") as f:
    f.write('(fp_lib_table (version 7)\n'
            f'  (lib (name "{design.LIB}")(type "KiCad")(uri "${{KIPRJMOD}}/{design.LIB}.pretty")(options "")(descr "Project footprints"))\n)\n')
print("wrote project file")

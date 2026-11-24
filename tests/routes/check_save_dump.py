"""Checks the cc_save.py dump of file 1 written by the `save` route (box 2 of an empty card; a new file takes the lowest free number): every field lands at its expected save-record offset.
Usage: check_save_dump.py <dump.json>"""
import json
import sys

d = json.load(open(sys.argv[1], encoding="utf-8"))
p = d["preview"]
h = bytes.fromhex(d["raw_head_hex_0x000_0x5AC"])
errs = []


def need(cond, msg):
    if not cond:
        errs.append(msg)


need(p["title_sjis"].endswith("０２"), "title does not end with the fullwidth file number 02: %r" % p["title_sjis"])
need(h[0x28:0x34] == bytes.fromhex("835a838b8357838500000000"), "preview leader name %a: not the name record (Serge's default) in Shift-JIS" % p["leader_name"])
need(p["party_ids"][0] == 0 and p["party_ids"][1] == 1, "preview party %r" % p["party_ids"])
need(p["location"] == 27, "location word %r (alu_a01f = room id 27)" % p["location"])
need(p["progression"] == 44, "progression (head 0x40) %r, expected the SCENARIO value 44" % p["progression"])
need(h[0x188] == 44 and h[0x189] == 0, "variable 0 at head 0x188 is %d" % h[0x188])
need(h[0x3F] == h[0x5A4], "preview stars 0x3F != 0x5A4")
need(h[0x59C] == 0 and h[0x59D] == 1, "party at 0x59C: %r" % list(h[0x59C:0x59F]))
need(h[0x194] == 0 and h[0x196] == 1, "hidden party copy (variables 0x0C / 0x0E) %r" % [h[0x194], h[0x196]])
c0 = d["characters"][0]
need(c0["max_hp"] > 0 and c0["hp"] > 0, "Serge has no HP")
need(c0["weapon"] != 0 and c0["armor"] != 0, "Serge gear missing")
need(h[0] == 0x53 and h[1] == 0x43, "header frame magic")
if errs:
    print("save dump check FAILED:\n  " + "\n  ".join(errs))
    sys.exit(1)
print("save dump check OK: title, leader, party, location 27, progression 44, variables at 0x188, Serge record")

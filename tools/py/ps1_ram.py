"""Guest RAM map of the original PlayStation game for the reference tools, one table per build.

The Japanese executable (SLPS_023.64; RDE and the SLPM re-release carry the same one) and the USA executable (SLUS_010.41)
are different builds: code, data and the overlay load bases moved. The tools name an address instead of writing it, and the
name resolves in the table of the build that is running:

    from ps1_ram import RAM
    RAM.use("usa")                       # ps1ref.Session does this from the disc it boots
    g.poke(RAM.btl_id, 0x200 | scene, 2)

A saved state carries its own map (`ps1_state.State(...).map`), chosen from the executable image in its RAM, so states of
both builds can be read side by side. Struct layouts (camera, warp globals, entity table) are the same in both builds; the
battle unit record keeps its field offsets up to +0x314 but is 0x28 bytes longer on the USA build, so a unit is reached through
`RAM.unit(i, offset)` (or the names unit0 .. unit8), never with a fixed stride.
"""
import os
import struct

# name -> (japan, usa)
_TABLE = {
    # main executable: pad, overlay and story state
    "pad_raw": (0x800856A0, 0x8007EAA8),          # raw 34-byte pad-1 buffer: [0] status [1] type [2..3] buttons (active low)
    "pad_reader": (0x8002B62C, 0x8002B6C0),       # per-frame pad reader; the frame hook (one hit = one game frame)
    "pad_held": (0x800855E0, 0x8007E9E8),         # decoded pad words: held, pressed, released, repeat (u16 each)
    "ovl_resident": (0x800856F0, 0x8007EAF8),     # nonzero while an effect overlay is resident
    "loc": (0x80077C26, 0x80070EDE),              # current room (location) id, u16
    "script_vars": (0x80077C20, 0x80070ED8),      # the saved script-variable block (field variable n at +2n)
    "btl_id": (0x80075906, 0x8006EBBE),           # battle id the next encounter starts, u16
    "area": (0x800790FC, 0x80072404),             # area word that picks the battlefield, u16
    "party": (0x800786F4, 0x800719AC),            # party character ids, 3 bytes
    "fx_load": (0x800720F8, 0x8006B474),          # effect pack load state
    "camera_eye": (0x80071F68, 0x8006B214),       # s16 x3
    "camera_target": (0x80071F70, 0x8006B21C),    # s16 x3
    "camera_roll": (0x80071F80, 0x8006B22C),
    "camera_h": (0x80071F84, 0x8006B230),
    "fog_far": (0x8008DBB2, 0x8007EDE2),
    "fog_rgb": (0x8008DBB4, 0x8007EDE4),
    "fog_near": (0x8008DBB8, 0x8007EDE8),
    "fog_on": (0x800721E0, 0x8006B30C),
    "warp": (0x800720D8, 0x8006B454),             # screen-copy warp globals: +0 on, +8 angles, +0x10 translation, +0x18 colour, +0x1C offset, +0x24 abr, +0x27 / +0x28 slow
    "effect_registry": (0x800721E4, 0x8006B490),  # head of the effect resource list
    "effect_work_end": (0x80072118, 0x8006B244),  # sentinel at the end of the effect task work area
    "cb_object": (0x80038E60, 0x80043B30),        # tick callback of a particle object task
    "cb_seq": (0x8002A26C, 0x8002A300),           # tick callback of a sequence task
    # mode overlay (field / battle / title) and the menu and effect overlays: the ID word sits at the load base
    "ovl_id": (0x800A9F00, 0x800A3308),
    "menu_ovl_id": (0x801D2800, 0x801D2800),
    "effect_ovl": (0x801C5E00, 0x801C5E00),
    # battle overlay state
    "btl_frame": (0x800E0518, 0x800DAEF0),        # battle refresh counter
    "act_element": (0x800E05DA, 0x800DAFB2),      # element of the selected action, u16
    "act_actor": (0x800E0634, 0x800DB00C),
    "act_targets": (0x800E0637, 0x800DB00F),      # current action target list (0xFF end)
    "ui_0642": (0x800E0642, 0x800DB01A),
    "pc_0649": (0x800E0649, 0x800DB021),
    "menu_064a": (0x800E064A, 0x800DB022),
    "tgt_064b": (0x800E064B, 0x800DB023),
    "cur_064e": (0x800E064E, 0x800DB026),
    "tgt_0650": (0x800E0650, 0x800DB028),
    "e0652": (0x800E0652, 0x800DB02A),
    "e065a": (0x800E065A, 0x800DB032),
    "act_kind": (0x800E0666, 0x800DB03E),
    "act_str": (0x800E0667, 0x800DB03F),
    "act_elem": (0x800E0668, 0x800DB040),
    "units": (0x800E06A0, 0x800DB078),            # unit records (stride per build): +0xB8 HP, +0x16C turn countdown, +0x16E gauge, +0x190 model, +0x19E stamina
    "phase_fc40": (0x800DFC40, 0x800DA618),
    "hint_f0ae": (0x800DF0AE, 0x800D947A),
    "target_masks": (0x800DE90C, 0x800D8D3C),     # u16 per unit
    "ring": (0x800DFF98, 0x800DA970),             # field element ring slots at +0, +4, +8
    "btl_vars_ptr": (0x800E2FC8, 0x800DDB30),     # pointer to the battle-script variable block
    "pgdat_ptr": (0x800E30A0, 0x800DDC28),        # element record of the current action
    "att_310e": (0x800E310E, 0x800DDC96),
    "cur_310f": (0x800E310F, 0x800DDC97),
    "unit_ctrl": (0x800E31B0, 0x800DDD38),        # unit control blocks, stride 0x30
    "pgdat_base": (0x800E3368, 0x800DDEF0),       # element record table base
    "e3372": (0x800E3372, 0x800DDEFA),
    "cb_damv": (0x800B07B8, 0x800A9BF0),          # damage digit flight stage callback
    "cb_dam1": (0x800B06D4, 0x800A9B0C),          # damage digit rest stage callback
    # field overlay state
    "fld_entities": (0x800E8894, 0x800E257C),     # pointer to the entity table, stride 0x278
    "fld_lead": (0x800E9F7C, 0x800E3C6C),         # index of the controlled entity, u8
    "fld_vars": (0x800EBAA8, 0x800E57A0),         # the field's live script-variable block
    # menu overlay (title menu, Load screen)
    "title_var": (0x801F377C, 0x801F31A4),
    "mt_mode": (0x801F5488, 0x801F4110),
    "mt_cur": (0x801F54A8, 0x801F4130),
    "mt_scroll": (0x801F54A0, 0x801F4128),
    "mt_5480": (0x801F5480, 0x801F4108),
    "mt_5478": (0x801F5478, 0x801F4100),
    "mt_547c": (0x801F547C, 0x801F4104),
    "ld_5010": (0x801F5010, 0x801F3C94),
    "ld_cur": (0x801F4FE4, 0x801F3C68),
    "ld_card": (0x801F502C, 0x801F3CB0),
}

# PCs of the sound-request calls the audio trace breaks on (pointer play, sfx, ...), and the per-frame animation sound calls.
_SOUND_PCS = ((0x800368C4, 0x8004A118, 0x8004A168, 0x8004F5C8, 0x8004E9D0),
              (0x8003F940, 0x8004A18C, 0x8004A1DC, 0x8004F688, 0x8004EA90))
_ANIM_PCS = ((0x800B9DDC, 0x800B9F90), (0x800B3214, 0x800B33C8))

REGIONS = ("japan", "usa")
# First 16 bytes of the executable image (guest 0x80010000) of each build.
EXE_SIGNATURES = {"japan": bytes.fromhex("0800e003000000000780023c803b4224"),
                  "usa": bytes.fromhex("0800e003000000000780023c28ce4224")}
UNIT_STRIDES = (0x410, 0x438)
UNIT_COUNT = 9
UNIT_HP, UNIT_COUNTDOWN, UNIT_GAUGE, UNIT_MODEL, UNIT_STAMINA = 0xB8, 0x16C, 0x16E, 0x190, 0x19E


class RamMap:
    """Attribute access to one build's addresses (`m.btl_id`); `use()` switches the build in place."""

    def __init__(self, region="japan"):
        self.use(region)

    def use(self, region):
        region = normalize(region)
        k = REGIONS.index(region)
        self.region = region
        for name, pair in _TABLE.items():
            setattr(self, name, pair[k])
        self.sound_pcs = _SOUND_PCS[k]
        self.anim_pcs = _ANIM_PCS[k]
        self.unit_stride = UNIT_STRIDES[k]
        for i in range(UNIT_COUNT):
            setattr(self, "unit%d" % i, self.unit(i))
        return self

    def unit(self, index, offset=0):
        return self.units + self.unit_stride * index + offset


def normalize(region):
    r = (region or "japan").lower()
    if r in ("jp", "ja", "jpn", "japan"):
        return "japan"
    if r in ("us", "usa", "en", "na"):
        return "usa"
    raise ValueError("unknown PS1 build %r (japan or usa)" % region)


def region_of_ram(ram):
    """Build whose executable image sits at guest 0x80010000 of a RAM dump; None if neither."""
    head = bytes(ram[0x10000:0x10010])
    return next((r for r, sig in EXE_SIGNATURES.items() if head == sig), None)


def region_of_disc(path):
    """Build of a disc image, read from the executable name in its ISO root directory (raw 2352-byte .bin behind a .cue, or a
    2048-byte .iso); falls back to the region word in the file name, then OC_PS1_REGION."""
    name = os.path.basename(path or "").lower()
    try:
        image, sector = _image_of(path)
        with open(image, "rb") as fh:
            def read(lba, n=2048):
                fh.seek(lba * sector + (24 if sector == 2352 else 0))
                return fh.read(n)
            pvd = read(16)
            if pvd[1:6] == b"CD001":
                root_lba, root_len = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
                data = b"".join(read(root_lba + i) for i in range((root_len + 2047) // 2048))
                if b"SLUS_" in data:
                    return "usa"
                if b"SLPS_" in data or b"SLPM_" in data:
                    return "japan"
    except (OSError, ValueError, struct.error):
        pass
    if "usa" in name or "(u)" in name:
        return "usa"
    if "japan" in name or "(j)" in name:
        return "japan"
    return normalize(os.environ.get("OC_PS1_REGION"))


def _image_of(path):
    low = path.lower()
    if low.endswith(".cue"):
        for line in open(path, encoding="utf-8", errors="replace"):
            parts = line.strip().split('"')
            if line.strip().upper().startswith("FILE") and len(parts) >= 2:
                return os.path.join(os.path.dirname(path), parts[1]), 2352
        raise ValueError("no FILE line in " + path)
    if low.endswith(".bin"):
        return path, 2352
    if low.endswith(".iso"):
        return path, 2048
    raise ValueError("not a readable image: " + path)


RAM = RamMap(os.environ.get("OC_PS1_REGION") or "japan")

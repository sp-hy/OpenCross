"""Opcode table for the Chrono Cross field script (room event bytecode).

Original work; the opcode numbers, operand sizes and names were derived as a
*specification* and then validated/corrected against the game data (see cc_fieldscript.py stats).

Row format:  HEX | operand spec | NAME | conf | semantics
  HEX   : "NN" main opcode, or "FExx" for the 0xFE extended page
  conf  : C = confirmed (decodes cleanly in data and the handler is understood)
          I = inferred (single source, or name guessed from context)
          ? = length known, semantics unknown
Operand spec tokens (space separated, in byte order, after the opcode [and sub-opcode]):
  b   u8 raw            c   u8 character reference     d   u8 (small enum/decimal)
  w   u16 raw           U   u16 immediate-or-variable (bit15 set = immediate, value&0x7FFF)
  V   u16 variable id   J   u16 jump target (offset in code area)
  Shh signed operand: s16 literal if bit hh of the mode byte M is set, else variable id
  Thh like S but when the mode bit is clear the u16 is a U (bit15 immediate / variable)
  M   u8 mode byte holding the literal-flag bits for the S/T operands
  A4  4 ASCII chars
A spec of the form  sel:MASK{v:spec;v:spec;default}  chooses the layout from (byte1 & MASK).
"""

TABLE = r"""
00 | | STOP | C | End this script instance.
01 | J | JUMP | C | Unconditional goto (code-area offset).
02 | T80 T40 M J | IF_JUMP | C | Compare operand0 with operand1 using cond=(M&0xF); fall through when the comparison holds, else jump to J. cond: 0 ==, 1 !=, 2 >, 3 <, 4 >=, 5 <=, 6 and-nonzero, 7 !=, 8 or, 9 and, A xor-ish (7-A inferred).
03 | | OP03 | ? | Halts script like STOP (treated as terminator by all decoders); semantics unknown.
04 | | KILL_SCRIPT | I | Halts script, like a kill-script request.
05 | J | CALL | C | Call subroutine at J (return stack depth 4); ends with RETURN (0D).
06 | | NOP06 | I | Dummy / no-op.
07 | c d | RUN_SCRIPT_ASYNC | C | Start script (low nibble of d) of entity c; d high nibble = mode flags. Does not wait.
08 | c d | RUN_SCRIPT_08 | ? | As 07 with unknown mode.
09 | c d | RUN_SCRIPT_BLOCKING | C | Start entity script and wait until it finishes.
0A | | UPDATE_CHARACTER | I | Refresh current entity (apply model/position).
0B | | UPDATE_CHARACTER_STOP | I | Refresh then halt entity.
0C | | NOP0C | I | No-op.
0D | | RETURN | C | Return from CALL / end of subroutine.
0E | c b | SETUP_PC | C | Mark this entity as a playable-character slot (c = char id, b = unknown flag, usually 0x80).
0F | U | SETUP_NPC | C | Load NPC model number U for this entity.
10 | V S40 M | SET | C | var = value.
11 | V | SET_1 | C | var = 1.
12 | V | SET_0 | C | var = 0.
13 | V S40 M | ADD | C | var += value.
14 | V S40 M | SUB | C | var -= value.
15 | V S40 M | OR | I | var |= value.
16 | V S40 M | XOR | I | var ^= value.
17 | V | INC | C | var++.
18 | V | DEC | C | var--.
19 | V S40 M | AND | I | var &= value.
1A | V S40 M | OR_1A | ? | Behaves as |= (duplicate of 15).
1B | V S40 M | XOR_1B | ? | Behaves as ^= (duplicate of 16).
1C | V U | SHL | I | var <<= U.
1D | V U | SHR | I | var >>= U.
1E | V | RAND | C | var = rand().
1F | V U | RAND_MOD | C | var = rand() % U.
20 | V S40 M | MUL | C | var *= value.
21 | V S40 M | DIV | C | var /= value.
22 | V S40 S20 M | SIN | I | var = sin(a, b) (angle 4096/turn, radius b).
23 | V S40 S20 M | COS | I | var = cos(a, b).
24 | V S40 S20 M | TAN | I | var = tan(a, b).
25 | S80 S40 M U | SET_POSITION_XZ | I | Set entity X, Z (signed) and U (unsigned, probably Y/height).
26 | S80 S40 S20 M | SET_POSITION_XYZ | I | Set entity X, Y, Z (signed).
27 | U | OP27 | ? |
28 | U | OP28 | ? |
29 | U | OP29 | ? |
2A | S80 M | OP2A | ? |
2B | b | ENABLE_CONTROLS | I | Give player control (b = mask).
2C | | DISABLE_CONTROLS | I | Remove player control.
2D | U | WAIT | C | Wait U frames.
2E | c | TURN_TO_ENTITY | I | Face entity c.
2F | b | SET_ROTATION_SIMPLE | I | Set facing from byte (0..255 = 360 deg).
30 | U | SET_ROTATION | I | Set facing (4096 = 360 deg).
31 | c U | TURN_TO_DIRECTION | I | Turn entity c to angle U.
32 | c U | OP32 | ? | Turn-related.
33 | c c | TURN_TO_ENTITY_2 | I | Entity c0 faces entity c1.
34 | b | TOGGLE_COLLISION | I | Enable/disable entity collision.
35 | b c | SET_HIDDEN | I | Hide entity (b flag).
36 | b c | SET_VISIBLE | I | Show entity.
37 | b | OP37 | ? |
38 | b | OP38 | ? |
39 | b | OP39 | ? |
3A | | WAIT_FRAME | C | Yield for one frame.
3B | | STOP_EXEC | I | Stop script execution; treated as a terminator (usually followed by a dead STOP).
3C | U | SET_ANIMATION | C | Play animation U on this entity.
3D | b U | SET_WALK_ANIMATION | I | Animation used for walking (slot b).
3E | U | OP3E | ? |
3F | sel:FF{0:b S80 S40 S20 M;b} | WALK_TO | C | b==0: start walking to (x,y,z); b!=0: wait for walk to finish.
40 | sel:FF{0:b S80 S40 S20 M U;b} | WALK_TO_2 | C | As 3F plus U (speed/time); b!=0 = wait.
41 | U | SET_MOVE_SPEED | C | Movement speed.
42 | sel:FF{1:b;b S80 S40 M U} | WALK_TO_XZ | I | b==1: wait; else walk to (x,z) with U (speed).
43 | sel:FF{1:b;b S80 S40 M U U} | WALK_TO_XZ_2 | I | b==1: wait; else walk to (x,z); two U params (frames/speed).
44 | b c | OP44 | ? | Entity-related; always used as the pair 44 00 c / 44 01 c (second = wait).
45 | sel:FF{0:b c U;b c} | OP45 | ? | Pair 45 00 c U / 45 01 c (second = wait).
46 | sel:FF{1:b;b U U} | MOVE | I | b==1: wait for move; else move direction U for U frames.
47 | b | OP47 | ? |
48 | c V V V | GET_POSITION | C | var0..2 = X,Y,Z of entity c.
49 | V | GET_TARGET_ROTATION | I | var = target rotation & 0xFFF.
4A | c V | GET_ROTATION | I | var = rotation of entity c.
4B | V | DEBUG | I | Debug print of variable.
4C | V S40 S20 S10 S08 M | GET_DISTANCE_2D | I | V = sqrt(dx*dx + dz*dz) of the two S-pairs.
4D | V S40 S20 S10 S08 S04 S02 M | OP4D | ? | Six-operand query returning var.
4E | V c c | ENTITY_DISTANCE | C | var = distance between entities.
4F | V c c | OP4F | ? | Same shape as 4E (var, entity, entity).
50 | w J | JUMP_IF_NOT_KEY | I | Jump to J if (input keys & w) == 0.
51 | w J | JUMP_IF_NOT_KEY2 | I | Jump to J if (scriptVar2 & w) == 0.
54 | | CLEAR_SCRIPTVAR2 | I | scriptVar2 = 0.
55 | b J | OP55 | ? | Conditional jump with byte argument.
56 | b J | OP56 | ? | Conditional jump with byte argument.
59 | | OP59 | ? |
5A | U U | SET_NEXT_ROOM | C | Map jump: U0 = room id, U1 = entry parameter (ROOM_PARAM).
5B | sel:03{3:b;b S80 S40 S20 S10 M} | OP5B | ? | (b&3)==3: wait; else 4 operands.
5C | sel:03{3:b;b S80 S40 S20 M U} | JUMP_MOVE | I | Jump-arc movement; (b&3)==3 waits.
5D | sel:FF{0:b S80 S40 S20 M;b} | SET_CAMERA_POSITION | I | b==0: move camera to x,y,z; b==1: wait. (Length is 9, followed by a separate 5D 01; an 11-byte reading does not fit the data.)
5E | sel:FF{0:b U;b} | SET_CAMERA_TO_ACTOR | I | b==0: target entity U; else wait.
5F | c | SET_CONTROLLED_ENTITY | I | Override which entity the player controls.
60 | | CLEAR_CONTROLLED_ENTITY | I |
61 | sel:FF{0:b c U;1:b;b} | CAMERA_FOLLOW | I | b==0: follow entity c (U speed); b==1: wait for camera.
62 | U U U U U | SETUP_DIALOG_WINDOW | C | x, y, rows, columns, flags (bit 0x40 = show face).
63 | U | OP63 | ? |
64 | S80 M | OP64 | ? |
65 | w | OP65 | ? |
66 | b S80 M | OP66 | ? |
67 | w | SET_SHADOW_COLOR | I | w = code-area offset of 4x4 bytes colour data.
68 | U U U U U | SET_SCREEN_RGB | C | Screen tint r,g,b,(2 more: time/flag).
69 | U J | JUMP_IF_SCENARIO_EQ | C | Jump if U == SCENARIO_FLAG.
6A | U J | JUMP_IF_SCENARIO_GT | C | Jump if U < SCENARIO_FLAG.
6B | U J | JUMP_IF_SCENARIO_NE | C | Jump if U != SCENARIO_FLAG.
6C | U | SET_SCENARIO | C | SCENARIO_FLAG = U.
6D | V | GET_SCENARIO | C | var = SCENARIO_FLAG.
6E | V V | SWAP | I | Swap two variables.
6F | w V U | ARRAY8_GET | I | var = byte_array[w][U] (w = code-area offset of array).
70 | w V U b | ARRAY16_GET | I | var = word_array[w][U]; b unknown.
71 | S80 M | SET_VAR_BIT | C | Set a bit in the persistent bit array.
72 | S80 M | CLEAR_VAR_BIT | C | Clear a bit.
73 | S80 M J | JUMP_IF_BIT_CLEAR | C | Jump to J when the bit is clear.
74 | S80 M c | OP74 | ? | Bit-array op with entity.
75 | b b U | SETUP_FLAG | ? |
76 | b b U | OP76 | ? |
77 | b b w | OP77 | ? |
78 | b b U | OP78 | ? |
79 | c U J | OP79 | ? | Entity test + jump.
7A | c U J | OP7A | ? | Entity test + jump.
7B | U | ADD_TO_PARTY | C | Add character U to the active party.
7C | | EXECUTE_CHARACTER_SCRIPT | ? |
7D | c | REMOVE_FROM_PARTY | C | Remove character from party.
7E | U | PLAY_MOVIE | C | Movie number U.
7F | | WAIT_MOVIE | C |
80 | U | BG_LAYER_ON | C | Enable background layer U (0..0x1F).
81 | U | BG_LAYER_OFF | C | Disable background layer U.
82 | b c U | AXIS_SCALE | I | Entity scale on axis b.
83 | S80 S40 S20 S10 M | OP83 | ? |
84 | S80 S40 S20 S10 S08 S04 M U U U b b b | SETUP_FORMATION | I | Party formation positions.
85 | | GO_TO_FORMATION | I |
86 | b | CAMERA_AUTO_FOLLOW | I |
87 | U U U | SET_UNDERWATER_RGB | I |
88 | U | PLAY_SONG | C | Start BGM U.
89 | U w | OP89 | ? | Song-related; 2 trailing bytes unused.
8A | | WAIT_SONG | C | Wait for BGM to finish.
8B | U U | OP8B | ? |
8C | U U | RECT_SIZE | I | Rectangle collision size.
8D | b | OP8D | ? |
8E | b | TOGGLE_RECT_COLLISION | I |
8F | U U U U U U U U | BG_WAVE_ON | I | Background wave effect (8 params).
90 | sel:FF{0:b U;b} | BG_WAVE_OFF | I |
91 | b | OP91 | ? |
92 | b U | OP92 | ? |
93 | b | OP93 | ? |
94 | U J | JUMP_IF_NOT_IN_PARTY | C | Jump if character U not in party.
95 | U J | OP95 | ? | Character test + jump.
96 | U | ADD_TO_ROSTER | C |
97 | U | REMOVE_FROM_ROSTER | C |
98 | V U | FIND_PARTY_SLOT | I | var = party slot of character U.
99 | V U | GET_PARTY_CHARACTER | I | var = character id in slot U.
9A | U J | OP9A | ? | Test + jump (u16 is a valid code offset in the data).
9B | A4 c | OP9B | ? | Effect related (4-char name + entity).
9C | A4 | OP9C | ? | Effect related.
9D | c | OP9D | ? |
9E | U | SET_DIALOG_FACE | C | Portrait for next dialog.
9F | | CLEAR_DIALOG_FACE | C |
A0 | b | OPA0 | ? |
A1 | c U U U | ENTITY_RGB | I | Entity colour tint.
A2 | c U | OPA2 | ? |
A3 | U U U | BATTLE_A3 | C | Battle with variable enemy set.
A4 | U | OPA4 | ? |
A5 | U | OPA5 | ? |
A6 | b | OPA6 | ? |
A7 | U U U U | OPA7 | ? |
A8 | b U | FADE | C | Screen fade.
A9 | c U | PLAY_ANIMATION | C | Play animation U on entity c.
AA | b c U | SET_AXIS_ROTATION | I |
AB | U U | OPAB | ? |
AC | c U | OPAC | ? |
AD | b | OPAD | ? |
AE | b | OPAE | ? |
AF | S80 S40 M U | OPAF | ? |
B0 | S80 M | SET_ROTATION_B0 | I |
B1 | b | OPB1 | ? |
B2 | sel:FF{0:b;1:b U;b} | OVERRIDE_FOLLOW | I |
B3 | c J | OPB3 | ? | Entity test + jump.
B4 | U J | JUMP_IF_NO_ITEM | C | Jump if item U not in inventory.
B5 | U | OPB5 | ? |
B6 | A4 | OPB6 | ? |
B7 | S80 M | OPB7 | ? |
B8 | U | OPB8 | ? |
B9 | sel:FF{0:b c U;b c} | OPB9 | ? | Pair B9 00 c U / B9 01 c (second = wait).
BA | sel:FF{0:b U U U;1:b;b} | OPBA | ? |
BB | b U | RECEIVE_ITEM | C | b==0 give item, b!=0 take; U = item id (0x200 element, 0x400 key item flag bits).
BC | b b b b J | CHECK_MONEY | I | Jump if money check fails.
BD | b U | MONEY | I | Add/remove money.
BE | | OPBE | ? |
BF | sel:FF{0:b c;b} | OPBF | ? | b==0: start (entity c); b==1: 2-byte follow-up (wait?). Always seen as the pair BF 00 c, BF 01 (not a single 5-byte op).
C0 | c | FREE_EXTRA_ANIMATION | I |
C1 | C1 | LOAD_ANIMATION | I | b==0: load animation U for entity c (c==FF = party leader); b==1: wait. (Length is 5 even when c==FF, not 7.)
C2 | U U U U | SET_LAYER_OPACITY | I |
C3 | V V | GET_CAMERA_POSITION | I |
C4 | c | WAIT_ANIMATION | I |
C5 | U U U | OPC5 | ? |
C6 | sel:FF{0:b U U;b U U U U} | PLAY_SFX | C | Sound effect id, volume, pan (+2).
C7 | U | OPC7 | ? |
C8 | U S40 S20 S10 M U U | OPC8 | ? |
C9 | U U U | BATTLE | C | Battle: battlescript, battlefield, flags (0x01 keep field music, 0x02 losable, 0x04 dragoon disguise, 0x10 no victory music, 0x20 keep battle music, 0x40 boss theme 0x31 as the encounter song).
CA | | OPCA | ? |
CB | | WAIT_ROTATION | I |
CC | c | WAIT_ROTATION_ENTITY | I |
CD | U J | OPCD | ? |
CE | U J | JUMP_IF_SCENARIO_LT | C | Jump if SCENARIO_FLAG < U.
CF | c U | OPCF | ? |
D0 | J | OPD0 | ? | Jump.
D1 | U | LOAD_MODEL_D1 | I | Variant of 0F.
D2 | | OPD2 | ? |
D3 | A4 | PLAY_EFFECT | I | 4-char effect name.
D4 | U U | PLAY_MOVIE_SKIPPABLE | I |
D5 | V U | GET_ITEM_COUNT | I |
D6 | c U | ATTACH_TO_ENTITY | I |
D7 | A4 | OPD7 | ? | Effect related.
D8 | c U U J | OPD8 | ? |
D9 | U U | SETUP_CHARACTER_MODEL | I |
DA | | WAIT_CHARACTER_MODEL | I |
DB | U V | GET_CHARACTER_ID | I |
DC | U U | MAP_JUMP_DC | I | Variant of 5A.
DD | sel:FF{0:b U;b} | DATA_READ | I | b==0: read data block U; else wait.
DF | S80 S40 M b b b | OPDF | ? | 9 bytes; third word not an imm/var operand in the data.
E0 | w b | DIALOG | C | Show message w (index into the room dialogue block), b = flags.
E1 | b | MULTI_CHOICE | C | Selection; b = (first<<4)|last choice. Result in var 0x04.
E2 | | WAIT_DIALOG | C |
E3 | c w b | CHARACTER_DIALOG | C | Message w spoken by entity c.
E4 | c w b | OPE4 | I | Like E3.
E5 | U J | OPE5 | ? |
E6 | U J | JUMP_IF_NOT_RECRUITED | I | Jump if character U not recruited.
E7 | A4 | OPE7 | ? | 4 ASCII chars (like 'had1'); related to 9B/9C/D3/D7 effect names.
E8 | U J | OPE8 | I | Character test + jump (pattern: E8 c,J ; ADD_TO_ROSTER c) - probably jump if recruited.
E9 | w J | JUMP_IF_FRAME | C | Jumps when bit (w & 0x1F) of the owned-frames mask is clear.
FE00 | b | OPFE00 | ? |
FE01 | b | OPFE01 | ? |
FE02 | U | OPFE02 | ? |
FE03 | b S80 M | OPFE03 | ? |
FE04 | b | OPFE04 | ? |
FE05 | U | SET_SCRIPTVAR1 | I | scriptVar1 = U.
FE06 | b | OPFE06 | ? |
FE07 | b | OPFE07 | ? |
FE09 | c U | OPFE09 | ? |
FE0A | c U | OPFE0A | ? |
FE0C | b c | FORCE_DIRECTION | I |
FE0D | U | OPFE0D | ? |
FE0E | U | OPFE0E | ? |
FE0F | c U | OPFE0F | ? |
FE10 | | BATTLE_SET | I | Set by A3/C9.
FE11 | U | OPFE11 | ? |
FE12 | U U U | OPFE12 | ? |
FE13 | U U U | OPFE13 | ? |
FE14 | b | OPFE14 | ? |
FE15 | U | OPFE15 | ? |
FE17 | b | OPFE17 | ? |
FE18 | b | OPFE18 | ? |
FE19 | b | OPFE19 | ? |
FE1A | | OPFE1A | ? |
FE1B | U | SET_CURRENT_FRAME | I | CURRENT_FRAME variable (0x44) write.
FE1C | | UPDATE_CURRENT_FRAME | I |
FE1D | U U | OPFE1D | ? |
FE21 | U U U | SETUP_SHAKE | C | Screen shake: horizontal, vertical, time to max.
FE22 | U U U | CHANGE_SFX_VOLUME | C |
FE23 | U U U | OPFE23 | ? |
FE25 | b | OPFE25 | ? |
FE29 | U | OPFE29 | ? |
FE2A | b | OPFE2A | ? |
FE2B | b U | OPFE2B | ? |
FE2C | b | OPFE2C | ? |
FE2D | U | OPFE2D | ? |
FE2E | | OPFE2E | ? |
FE2F | | OPFE2F | ? |
FE30 | S80 S40 S20 M | OPFE30 | ? | Three signed operands (mode byte 0xE0 in data).
FE31 | c | OPFE31 | ? |
FE32 | | OPFE32 | ? |
FE33 | b | OPFE33 | ? |
FE34 | S80 S40 S20 S10 S08 M | OPFE34 | ? |
FE35 | S80 M | OPFE35 | ? |
FE36 | U | HEAL_GROUP | C | Heal party group.
FE37 | V U | GET_CHAR_LIFE | I |
FE38 | S80 U M | OPTIONAL_LIFE | I |
FE3A | S80 U M | OPFE3A | ? |
FE3B | V U | GET_CHAR_STATUS_BYTE | I |
FE3C | U | SET_SELF_258 | I | PS1 handler consumes FE 3C U (4 bytes); it is not 7.
FE3D | U U | SET_STATUS | C | Inflict status (|= mask: 1 poison, 8 burn, 16 sprain, 32 flu).
FE3E | U U | CURE_STATUS | C | Cure status (&= ~mask).
FE3F | | OPFE3F | ? |
FE40 | U | OPFE40 | ? |
FE41 | b | OPFE41 | ? |
FE42 | b | OPFE42 | ? |
FE43 | U U | OPFE43 | ? |
FE44 | | DRAW_SYNC | C |
FE45 | sel:FF{0:b c;b c b b} | OPFE45 | ? | Possibly 5 bytes if byte1!=0, else 3; unverified.
FE46 | S80 S40 S20 M | OPFE46 | ? |
FE47 | S80 S40 S20 M | OPFE47 | ? |
FE48 | b U w w w | OPFE48 | ? |
FE49 | S80 S40 M | OPFE49 | ? |
FE4A | U | RENAME_CHARACTER | C | Naming screen for character U.
FE4B | b | OPFE4B | ? |
FE4C | c | OPFE4C | ? |
FE4E | b | OPFE4E | ? |
FE4F | b U | OPFE4F | ? |
FE50 | | OPFE50 | ? |
FE52 | U | OPFE52 | ? |
FE53 | S80 S40 M c | HEAD_ANGLE | I |
FE55 | c c | LOOK_AT_ENTITY | I |
FE56 | c | HEAD_DEFAULT | I |
FE57 | S80 M c | FACING_VEC_SET_TARGET | I |
FE59 | | OPFE59 | ? |
FE5A | b | OPFE5A | ? |
FE5B | | PARTY_SCREEN | C | Character exchange / party screen.
FE5C | b c U | OPFE5C | ? |
FE5D | c U | OPFE5D | ? |
FE5E | U | ELEMENT_SHOP | C |
FE60 | c U | OPFE60 | ? |
FE61 | | OPEN_MENU | C |
FE62 | c U | HEAD_SPEED | I |
FE63 | | OPFE63 | ? | Stops script if a variable set.
FE64 | b c | OPFE64 | ? |
FE65 | sel:FF{0:b U;b} | CHANGE_DISC | C | Disc change prompt.
FE66 | | OPFE66 | ? |
FE67 | U | OPFE67 | ? |
FE68 | U | WEAPON_SHOP | C | Blacksmith / weapon shop.
FE69 | b | OPFE69 | ? |
FE6B | b | OPFE6B | ? |
FE6C | b | OPFE6C | ? |
FE6D | | RECOVER_STARS | I |
FE6E | V | OPFE6E | ? |
FE6F | b | OPFE6F | ? |
FE70 | c | OPFE70 | ? |
FE71 | b | ALT_PORTRAIT | I |
FE72 | | OPFE72 | ? |
FE73 | U U | LEARN_LIMIT_2 | I |
FE74 | U U | LEARN_LIMIT_1 | I |
FE75 | U | OPFE75 | ? | Possibly a grand slam screen.
FE76 | V | OPFE76 | ? |
FE77 | w | OPFE77 | ? |
FE78 | U U | OPFE78 | ? | Stalls until a condition is met.
FE79 | U U | LEARN_LIMIT_0 | I |
FE7A | U | ADD_FRAME_TO_COLLECTION | I |
FE7B | U | TOGGLE_LIMIT_MODE | I |
FE7C | U U | OPFE7C | ? |
FE7D | U | OPFE7D | ? |
FE7E | | OPFE7E | ? |
FE80 | b | OPFE80 | ? |
FE81 | b | OPFE81 | ? |
FE82 | V | GET_STARS_TOTAL | I | PS1 handler reads a u16 var id (4 bytes total).
FE83 | | OPFE83 | ? | Seen once (sui_a05f) directly before RECEIVE_ITEM; length taken as 2 (fits the surrounding code).
FE84 | | NEW_GAME_PLUS_LIST | C | Keeps the cleared game, selects launcher mode 0x30 (ending room only).
FE85 | | CLEARED_SAVE | C | Strips gear / grids, var 0x7E = 3, selects the save list (mode 4).
"""

# ---------------------------------------------------------------------------
# Implementation
# ---------------------------------------------------------------------------
import argparse
import collections
import os
import struct
import sys
import zipfile

HEADER_SIZE = 0x80          # 32 x u32 "local variable" table
ENTITY_STRIDE = 0x40        # 32 x u16 script pointers
SCRIPT_NAMES = {0: "INIT", 1: "UPDATE", 2: "ON_ACTION", 3: "ON_COLLISION"}

GLOBAL_VARS = {
    0x02: "ROOM_PARAM", 0x04: "MULTI_CHOICE_RESULT", 0x06: "MAP_ID", 0x08: "PREVIOUS_MAP_ID",
    0x0C: "PARTY_MEMBER_0", 0x0E: "PARTY_MEMBER_1", 0x10: "PARTY_MEMBER_2",
    0x18: "DIGIT_ENTRY_RESULT", 0x2A: "USED_KEY_ITEM", 0x32: "BATTLE_RESULT",
    0x34: "DEBUG_MODE", 0x44: "CURRENT_FRAME", 0x4C: "SAVE_RESULT",
}
CHAR_NAMES = ["Serge", "Kid", "Guile", "Norris", "Nikki", "Viper", "Riddel", "Karsh", "Zoah", "Marcy",
              "Korcha", "Luccia", "Poshul", "Razzly", "Zappa", "Orcha", "Radius", "Fargo", "Macha",
              "Glenn", "Leena", "Miki", "Harle", "Janice", "Draggy", "Starky", "Sprigg", "Mojo",
              "Turnip", "NeoFio", "Greco", "Skelly", "Funguy", "Irenes", "Mel", "Leah", "Van",
              "Sneff", "Steena", "Doc", "Grobyc", "Pierre", "Orlha", "Pip"]
COND = {0: "==", 1: "!=", 2: ">", 3: "<", 4: ">=", 5: "<=", 6: "&", 7: "!=7", 8: "|", 9: "&9", 10: "~"}
TERMINATORS = {(0x00,), (0x03,), (0x04,), (0x0D,), (0x01,), (0xD0,)}


class DecodeError(Exception):
    pass


# ----- LZSS ("sszl") --------------------------------------------------------
def lzss_decode(data):
    """Chrono Cross LZSS.  12-byte header 'sszl', u32 length, u32 ?; then MSB-first
    bit stream: 1 + 8-bit literal | 0 + 12-bit ring position + 4-bit count (copy
    count+2).  Ring buffer 4096 bytes, zero initialised, write pointer starts at
    index 1 (verified against the data: with start 0 or 0xFEE the field-script
    entity tables come out corrupted; with 1 they are clean)."""
    if data[:4] != b"sszl":
        raise ValueError("not sszl")
    want = struct.unpack_from("<I", data, 4)[0]
    if want == 0:
        return b""
    src = data[12:] + b"\0\0\0\0"
    nbits = (len(data) - 12) * 8
    ring = bytearray(4096)
    wp = 1
    out = bytearray()
    bp = 0
    unpack = struct.unpack_from
    while len(out) < want and bp + 9 <= nbits:
        w = unpack(">I", src, bp >> 3)[0]
        sh = bp & 7
        if (w >> (31 - sh)) & 1:
            c = (w >> (23 - sh)) & 0xFF
            bp += 9
            out.append(c)
            ring[wp] = c
            wp = (wp + 1) & 4095
        else:
            if bp + 17 > nbits:
                break
            v = (w >> (15 - sh)) & 0xFFFF if sh <= 15 else 0
            p = v >> 4
            n = (v & 15) + 2
            bp += 17
            for _ in range(n):
                c = ring[p]
                p = (p + 1) & 4095
                out.append(c)
                ring[wp] = c
                wp = (wp + 1) & 4095
    return bytes(out[:want])


# ----- room access ----------------------------------------------------------
def default_zip_path():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "..", "..", "data", "cdrom.dat"))


class Archive:
    def __init__(self, path=None):
        self.z = zipfile.ZipFile(path or default_zip_path())
        self.rooms = sorted(n[len("map/mapbin/"):-4] for n in self.z.namelist()
                            if n.startswith("map/mapbin/") and n.endswith(".bin"))

    def cpt(self, room):
        d = self.z.read("map/mapbin/%s.bin" % room)
        n = struct.unpack_from("<I", d, 0)[0]
        offs = struct.unpack_from("<%dI" % (n + 1), d, 4)
        return [d[offs[i]:offs[i + 1]] for i in range(n)]

    def script_blob(self, room):
        subs = self.cpt(room)
        return lzss_decode(subs[3])

    def dialogue_blob(self, room):
        subs = self.cpt(room)
        return lzss_decode(subs[8]) if len(subs) > 8 and subs[8][:4] == b"sszl" else b""


# ----- operand spec parsing -------------------------------------------------
class OpDef:
    __slots__ = ("key", "name", "spec", "conf", "sem")


def _parse_table():
    ops = {}
    for line in TABLE.strip().splitlines():
        p = [x.strip() for x in line.split("|", 4)]
        o = OpDef()
        o.key = int(p[0], 16)          # 0xNN or 0xFExx
        if len(p[0]) == 4:
            o.key = 0xFE00 | int(p[0][2:], 16)
        o.spec, o.name, o.conf, o.sem = p[1], p[2], p[3], p[4] if len(p) > 4 else ""
        ops[o.key] = o
    return ops


OPS = _parse_table()


def _resolve_spec(spec, buf, pos, nprefix):
    """Return the flat token list for the spec at buf[pos] (pos = opcode byte)."""
    if spec == "C1":
        b1 = buf[pos + nprefix] if pos + nprefix < len(buf) else 0
        if b1 == 0:
            return ["b", "c", "U"]
        return ["b"]
    if spec.startswith("sel:"):
        mask = int(spec[4:6], 16)
        body = spec[spec.index("{") + 1:spec.rindex("}")]
        parts = body.split(";")
        arms, default = parts[:-1], parts[-1]
        b1 = buf[pos + nprefix] if pos + nprefix < len(buf) else 0
        for arm in arms:
            v, s = arm.split(":")
            if (b1 & mask) == int(v, 16):
                return s.split()
        return default.split()
    return spec.split()


_WIDTH = {"b": 1, "c": 1, "d": 1, "M": 1, "w": 2, "U": 2, "V": 2, "J": 2, "A4": 4}


def _width(tok):
    if tok in _WIDTH:
        return _WIDTH[tok]
    if tok[0] in "ST":
        return 2
    raise ValueError(tok)


class Insn:
    __slots__ = ("off", "length", "op", "opdef", "ops", "targets", "flow", "raw", "cond")

    def __init__(self):
        self.targets = []
        self.ops = []
        self.cond = None


def decode_insn(code, off):
    """Decode one instruction at code[off:].  Raises DecodeError."""
    n = len(code)
    if off >= n:
        raise DecodeError("past end @%04X" % off)
    op = code[off]
    nprefix = 1
    if op == 0xFE:
        if off + 1 >= n:
            raise DecodeError("truncated FE @%04X" % off)
        key = 0xFE00 | code[off + 1]
        nprefix = 2
    else:
        key = op
    od = OPS.get(key)
    if od is None:
        raise DecodeError("unknown opcode %s @%04X" % (("FE %02X" % (key & 0xFF)) if op == 0xFE else "%02X" % op, off))
    toks = _resolve_spec(od.spec, code, off, nprefix)
    pos = off + nprefix
    offs = []
    moff = None
    for t in toks:
        offs.append(pos)
        if t == "M":
            moff = pos
        pos += _width(t)
    if pos > n:
        raise DecodeError("operands run past end @%04X" % off)
    ins = Insn()
    ins.off = off
    ins.length = pos - off
    ins.op = key
    ins.opdef = od
    ins.raw = code[off:pos]
    mode = code[moff] if moff is not None else 0
    flow = "next"
    for t, p in zip(toks, offs):
        if t in ("b", "d"):
            ins.ops.append(("byte", code[p]))
        elif t == "c":
            ins.ops.append(("char", code[p]))
        elif t == "w":
            ins.ops.append(("word", struct.unpack_from("<H", code, p)[0]))
        elif t == "U":
            v = struct.unpack_from("<H", code, p)[0]
            ins.ops.append(("imm", v & 0x7FFF) if v & 0x8000 else ("var", v))
        elif t == "V":
            ins.ops.append(("var", struct.unpack_from("<H", code, p)[0]))
        elif t == "J":
            v = struct.unpack_from("<H", code, p)[0]
            ins.ops.append(("jump", v))
            ins.targets.append((v, "call" if key == 0x05 else "jump"))
        elif t == "A4":
            ins.ops.append(("str", bytes(code[p:p + 4]).decode("latin-1")))
        elif t == "M":
            ins.ops.append(("mode", code[p]))
        elif t[0] in "ST":
            mask = int(t[1:], 16)
            if mode & mask:
                ins.ops.append(("imm", struct.unpack_from("<h", code, p)[0]))
            else:
                v = struct.unpack_from("<H", code, p)[0]
                if t[0] == "T":
                    ins.ops.append(("imm", v & 0x7FFF) if v & 0x8000 else ("var", v))
                else:
                    ins.ops.append(("var", v))
        else:
            raise ValueError(t)
    if key == 0x02:
        ins.cond = mode & 0xF
    if key in (0x00, 0x03, 0x04, 0x0D, 0x3B):
        flow = "end"
    elif key in (0x01, 0xD0):
        flow = "goto"
    ins.flow = flow
    return ins


# ----- whole script ---------------------------------------------------------
class Script:
    """A parsed field-script blob."""

    def __init__(self, blob, room=""):
        self.room = room
        self.blob = blob
        if len(blob) < HEADER_SIZE + 4:
            raise DecodeError("script too short")
        self.locals = struct.unpack_from("<32I", blob, 0)
        self.nent = struct.unpack_from("<I", blob, HEADER_SIZE)[0]
        base = HEADER_SIZE + 4
        if base + self.nent * ENTITY_STRIDE > len(blob):
            raise DecodeError("entity table past end")
        self.entities = []
        for i in range(self.nent):
            self.entities.append(list(struct.unpack_from("<32H", blob, base + i * ENTITY_STRIDE)))
        self.code_base = base + self.nent * ENTITY_STRIDE
        self.code = blob[self.code_base:]
        self.insns = {}        # offset -> Insn
        self.errors = []       # (offset, message)
        self.entries = {}      # offset -> list of (entity, idx)
        self.subs = set()
        self.labels = set()
        self.data_refs = {}    # offset -> kind  (67 palette / 6F,70 arrays)
        self._explore()

    def _explore(self):
        code = self.code
        for e, ptrs in enumerate(self.entities):
            for k, p in enumerate(ptrs):
                if p or k == 0:
                    self.entries.setdefault(p, []).append((e, k))
        work = list(self.entries)
        seen_starts = set()
        while work:
            start = work.pop()
            if start in seen_starts:
                continue
            seen_starts.add(start)
            off = start
            while True:
                if off in self.insns:
                    break
                if off >= len(code):
                    self.errors.append((start, "runs off end of code"))
                    break
                try:
                    ins = decode_insn(code, off)
                except DecodeError as ex:
                    self.errors.append((off, str(ex)))
                    break
                self.insns[off] = ins
                if ins.op in (0x67, 0x6F, 0x70):
                    self.data_refs[ins.ops[0][1]] = ins.opdef.name
                for t, kind in ins.targets:
                    if t >= len(code):
                        self.errors.append((off, "jump target %04X out of range" % t))
                    else:
                        work.append(t)
                        (self.subs if kind == "call" else self.labels).add(t)
                if ins.flow in ("end", "goto"):
                    break
                off += ins.length
        self._sweep_dead()
        # overlap check
        starts = sorted(self.insns)
        end_prev = -1
        for s in starts:
            if s < end_prev:
                self.errors.append((s, "overlapping instruction (target inside another instruction)"))
            end_prev = max(end_prev, s + self.insns[s].length)

    def _sweep_dead(self):
        """Unreferenced code: starting at the first byte of every gap between
        reachable instructions, decode straight-line until a terminator; keep the
        run only if it decodes cleanly and ends in a terminator.  A gap whose first
        run fails is treated as data (tables, palettes)."""
        code = self.code
        self.dead = {}
        cov = self.covered_bytes()
        n = len(code)
        i = 0
        while i < n:
            if cov[i]:
                i += 1
                continue
            run = []
            j = i
            ok = False
            while j < n and not cov[j]:
                try:
                    ins = decode_insn(code, j)
                except DecodeError:
                    break
                if any(cov[j:j + ins.length]):
                    break
                run.append(ins)
                j += ins.length
                if ins.flow != "next":
                    ok = True
                    break
            if ok:
                for ins in run:
                    self.dead[ins.off] = ins
                    for k in range(ins.off, ins.off + ins.length):
                        cov[k] = 1
                i = j
            else:
                while i < n and not cov[i]:   # skip the rest of this data gap
                    i += 1

    def covered_bytes(self, dead=False):
        cov = bytearray(len(self.code))
        src = list(self.insns.items()) + (list(self.dead.items()) if dead else [])
        for o, i in src:
            for k in range(o, min(o + i.length, len(cov))):
                cov[k] = 1
        return cov


# ----- dialogue -------------------------------------------------------------
# control code -> number of argument bytes; unknown
# codes are assumed to have none.
CTRL_ARGS = {0x04: 1, 0x05: 1, 0x06: 1, 0x07: 2, 0x08: 1, 0x0A: 1, 0x0B: 1, 0x0E: 1, 0x12: 1,
             0x15: 1, 0x16: 1, 0x17: 1, 0x18: 1, 0x19: 1, 0x1B: 1, 0x1C: 1, 0x1E: 1}
CTRL_NAMES = {0x00: "END", 0x01: "NL", 0x02: "PAGE", 0x03: "WAIT", 0x04: "DELAY", 0x05: "SPEED",
              0x06: "AUTOCLOSE", 0x07: "ITEM", 0x08: "NAME", 0x0A: "NUM", 0x0B: "NUM2", 0x0E: "COLOR",
              0x10: "DIGITS", 0x12: "ACCENT", 0x15: "SCRNAME", 0x16: "C16", 0x17: "C17",
              0x18: "PARTYNAME", 0x19: "SCRSTR", 0x1B: "BUTTON", 0x1C: "CASE", 0x1E: "SUFFIX",
              0x1F: "ACCENTPH"}


class Dialogue:
    def __init__(self, blob):
        self.blob = blob
        self.msgs = []     # (index, ptr, w1, w2, w3, bytes)
        if len(blob) < 8:
            return
        first = struct.unpack_from("<H", blob, 0)[0]
        n = first // 8
        recs = [struct.unpack_from("<HHHH", blob, i * 8) for i in range(n)]
        ptrs = sorted(set(r[0] for r in recs) | {len(blob)})
        for i, r in enumerate(recs):
            nxt = min([p for p in ptrs if p > r[0]] or [len(blob)])
            self.msgs.append((i, r[0], r[1], r[2], r[3], blob[r[0]:nxt]))

    @staticmethod
    def render(b):
        """Tokenise a message: control codes (< 0x20) decoded with their argument
        bytes, everything else shown as hex glyph codes (the JP glyph/relative
        alphabet is not recovered).  Returns (text, ok); ok = the token stream
        consumes the message exactly (no truncated argument) and any 00 (END)
        sits at the very end (optionally followed by one 01 byte)."""
        out = []
        i = 0
        n = len(b)
        run = []
        ok = True

        def flush():
            if run:
                out.append("".join("%02X" % x for x in run))
                run.clear()
        while i < n:
            c = b[i]
            if c >= 0xF8 and i + 1 < n:      # F8..FD: glyph-page prefix, next byte is part of the glyph
                run.append(c)
                run.append(b[i + 1])
                i += 2
                if len(run) >= 2:
                    out.append("".join("%02X" % x for x in run))
                    run.clear()
                continue
            if c >= 0x20:
                run.append(c)
                i += 1
                continue
            flush()
            na = 1 if c == 0x10 else CTRL_ARGS.get(c, 0)
            args = b[i + 1:i + 1 + na]
            if len(args) < na:
                ok = False
            i += 1 + na
            if c == 0x00:
                out.append("<END>")
                if n - i > 1 or (n - i == 1 and b[i] != 0x01):
                    ok = False
                if i < n:
                    out.append("<%02X>" % b[i])
                break
            out.append("<%s%s>" % (CTRL_NAMES.get(c, "C%02X" % c), "".join(" %02X" % x for x in args)))
        flush()
        return " ".join(out), ok


# ----- formatting -----------------------------------------------------------
def var_name(v):
    v &= 0xFFFF
    if v in GLOBAL_VARS:
        return "$" + GLOBAL_VARS[v]
    if v >= 0x400:
        return "$room[%d]" % ((v - 0x400) // 2)
    return "$%04X" % v


def char_name(c):
    if c == 0xFF:
        return "LEADER"
    if c == 0xFE:
        return "PC1"
    if c == 0xFD:
        return "PC2"
    if c == 0xFC:
        return "PC3"
    if c == 0xFB:
        return "THIS"
    if c >= 0x80:
        i = c - 0x80
        return CHAR_NAMES[i] if i < len(CHAR_NAMES) else "char%02X" % c
    return "ent%d" % c


def fmt_operand(o):
    k, v = o
    if k == "imm":
        return "#%d" % v
    if k == "var":
        return var_name(v)
    if k == "jump":
        return "L%04X" % v
    if k == "char":
        return char_name(v)
    if k == "byte":
        return "0x%02X" % v
    if k == "word":
        return "0x%04X" % v
    if k == "str":
        return '"%s"' % v
    if k == "mode":
        return "m=%02X" % v
    return str(v)


def fmt_insn(ins, dlg=None):
    parts = []
    for o in ins.ops:
        if o[0] == "mode":
            continue
        parts.append(fmt_operand(o))
    if ins.op == 0x02:
        a, b, tgt = parts[0], parts[1], parts[2]
        txt = "IF_JUMP   %s %s %s -> %s" % (a, COND.get(ins.cond, "cond%X" % ins.cond), b, tgt)
    else:
        txt = "%-9s %s" % (ins.opdef.name, ", ".join(parts))
    if dlg is not None and ins.op in (0xE0, 0xE3, 0xE4):
        idx = [v for k, v in ins.ops if k == "word"]
        if idx and idx[0] < len(dlg.msgs):
            t, _ = Dialogue.render(dlg.msgs[idx[0]][5])
            txt += "   ; msg[%d]: %s" % (idx[0], t[:90])
    return txt.rstrip()


def hexbytes(b):
    return " ".join("%02X" % x for x in b)


def disasm(script, dlg=None, entity=None, out=sys.stdout, show_data=True):
    """Listing in code-area offset order.  Entity filter limits output to code
    reachable from that entity's script pointers."""
    code_base = script.code_base
    print("; room %s  entities=%d  code_base=0x%X  code_len=0x%X  insns=%d  unreferenced=%d  errors=%d" % (
        script.room, script.nent, code_base, len(script.code), len(script.insns), len(script.dead),
        len(script.errors)), file=out)
    keep = None
    if entity is not None:
        keep = _reach(script, [p for p in script.entities[entity] if p])
    cov = script.covered_bytes(True)
    allins = dict(script.insns)
    allins.update(script.dead)
    last_end = None
    off = 0
    n = len(script.code)
    while off < n:
        ins = allins.get(off)
        if ins is None:
            j = off
            while j < n and j not in allins:
                j += 1
            if keep is None and show_data:
                if last_end is not None:
                    print("", file=out)
                for k in range(off, min(j, off + 4096), 16):
                    chunk = script.code[k:min(k + 16, j)]
                    print("  %04X: .data %s" % (k, hexbytes(chunk)), file=out)
            off = j
            last_end = None
            continue
        if keep is not None and off not in keep:
            off += ins.length
            continue
        if last_end is not None and off != last_end:
            print("", file=out)
        for (e, k) in script.entries.get(off, []):
            if entity is None or e == entity:
                print("; ---- entity %d script %d (%s)  ptr=%04X  file=%X" % (
                    e, k, SCRIPT_NAMES.get(k, "script%d" % k), off, code_base + off), file=out)
        if off in script.subs:
            print("sub_%04X:" % off, file=out)
        elif off in script.labels:
            print("L%04X:" % off, file=out)
        dead = "  ; unreferenced" if off in script.dead else ""
        print("  %04X: %-24s %s%s" % (off, hexbytes(ins.raw), fmt_insn(ins, dlg), dead), file=out)
        last_end = off + ins.length
        off += ins.length
    for off, msg in script.errors:
        print("; ERROR @%04X %s" % (off, msg), file=out)


def _reach(script, starts):
    seen = set()
    stack = list(starts)
    while stack:
        o = stack.pop()
        while o in script.insns and o not in seen:
            seen.add(o)
            ins = script.insns[o]
            for t, _ in ins.targets:
                stack.append(t)
            if ins.flow != "next":
                break
            o += ins.length
    return seen


# ----- commands -------------------------------------------------------------
def load_room(arc, room):
    room = os.path.basename(room)
    if room.endswith(".bin"):
        room = room[:-4]
    if room not in arc.rooms:
        raise SystemExit("unknown room %r (try: list)" % room)
    return Script(arc.script_blob(room), room)


def cmd_disasm(args):
    arc = Archive(args.data)
    sc = load_room(arc, args.room)
    dlg = Dialogue(arc.dialogue_blob(sc.room))
    ent = None if args.entity is None else int(args.entity, 0)
    disasm(sc, dlg, ent)


def cmd_dialogue(args):
    arc = Archive(args.data)
    room = os.path.basename(args.room).replace(".bin", "")
    d = Dialogue(arc.dialogue_blob(room))
    print("; room %s: %d messages, block %d bytes" % (room, len(d.msgs), len(d.blob)))
    for (i, ptr, a, b, c, raw) in d.msgs:
        t, ok = Dialogue.render(raw)
        print("[%3d] @%04X hdr=%04X %04X %04X%s  %s" % (i, ptr, a, b, c, "" if ok else " (!)", t))


def cmd_list(args):
    arc = Archive(args.data)
    print("\n".join(arc.rooms))


def spec_lengths(od):
    """Total instruction lengths (bytes incl. opcode/sub-opcode) a spec can produce."""
    pre = 2 if od.key > 0xFF else 1
    spec = od.spec
    if spec == "C1":
        arms = ["b c U", "b"]
    elif spec.startswith("sel:"):
        body = spec[spec.index("{") + 1:spec.rindex("}")]
        parts = body.split(";")
        arms = [x.split(":", 1)[1] for x in parts[:-1]] + [parts[-1]]
    else:
        arms = [spec]
    out = []
    for a in arms:
        n = pre + sum(_width(t) for t in a.split())
        if n not in out:
            out.append(n)
    return out


def cmd_optable(args):
    counts = collections.Counter()
    if args.counts:
        arc = Archive(args.data)
        for room in arc.rooms:
            sc = Script(arc.script_blob(room), room)
            for ins in sc.insns.values():
                counts[ins.op] += 1
    print("| Op | Len | Operands | Name | Conf | Uses | Semantics |")
    print("|---|---|---|---|---|---|---|")
    for k in sorted(OPS):
        o = OPS[k]
        hexs = "FE %02X" % (k & 0xFF) if k > 0xFF else "%02X" % k
        spec = o.spec.replace("|", "/") or "-"
        if spec.startswith("sel:"):
            spec = spec.replace("sel:", "by b1&").replace("{", " {").replace(";", "; ")
        lens = "/".join(str(x) for x in spec_lengths(o))
        print("| %s | %s | `%s` | %s | %s | %s | %s |" % (hexs, lens, spec, o.name, o.conf,
                                                          counts[k] if args.counts else "", o.sem.replace("|", "\\|")))


def cmd_stats(args):
    arc = Archive(args.data)
    hist = collections.Counter()
    unk = collections.Counter()
    unk_rooms = collections.defaultdict(list)
    fails = []
    tot_entities = tot_scripts = tot_entries = 0
    code_bytes = cov_bytes = dead_bytes = prefix_bytes = 0
    dead_hist = collections.Counter()
    rooms_ok = 0
    bad_rooms = []
    ent_hist = []
    n_insn = 0
    s_bit15 = collections.Counter()
    for room in arc.rooms:
        try:
            sc = Script(arc.script_blob(room), room)
        except Exception as ex:   # noqa
            fails.append((room, "load: %s" % ex))
            continue
        tot_entities += sc.nent
        ent_hist.append(sc.nent)
        tot_entries += len(sc.entries)
        tot_scripts += sum(1 for e in sc.entities for k, p in enumerate(e) if p or k == 0)
        for ins in sc.insns.values():
            hist[ins.op] += 1
            n_insn += 1
        cov = sc.covered_bytes()
        code_bytes += len(cov)
        cov_bytes += sum(cov)
        dead_bytes += sum(sc.covered_bytes(True)) - sum(cov)
        for ins in sc.dead.values():
            dead_hist[ins.op] += 1
        pre = min(sc.entries) if sc.entries else 0
        prefix_bytes += pre
        if sc.errors:
            bad_rooms.append(room)
            for off, msg in sc.errors:
                fails.append((room, msg))
                if msg.startswith("unknown opcode"):
                    key = msg.split()[2] if not msg.split()[2] == "FE" else "FE" + msg.split()[3]
                    unk[key] += 1
                    unk_rooms[key].append(room)
        else:
            rooms_ok += 1
    print("rooms: %d  decoded without error: %d  rooms with errors: %d" % (len(arc.rooms), rooms_ok, len(bad_rooms)))
    print("entities: %d (avg %.1f, max %d)  entry-point scripts: %d unique entries: %d" % (
        tot_entities, tot_entities / max(1, len(ent_hist)), max(ent_hist or [0]), tot_scripts, tot_entries))
    print("instructions decoded: %d  code bytes: %d  bytes covered by instructions: %d (%.2f%%)" % (
        n_insn, code_bytes, cov_bytes, 100.0 * cov_bytes / max(1, code_bytes)))
    print("  + unreferenced-but-clean code (linear sweep of gaps): %d bytes (%.2f%%); total decoded %.2f%%; "
          "pre-script table prefix (not code): %d bytes (%.2f%%)" % (
              dead_bytes, 100.0 * dead_bytes / max(1, code_bytes),
              100.0 * (cov_bytes + dead_bytes) / max(1, code_bytes), prefix_bytes,
              100.0 * prefix_bytes / max(1, code_bytes)))
    conf = collections.Counter(o.conf for o in OPS.values())
    print("semantics confidence over table: C=%d I=%d ?=%d" % (conf["C"], conf["I"], conf["?"]))
    print("main opcodes with no table entry (length unknown): %s" % " ".join(
        "%02X" % k for k in range(256) if k != 0xFE and k not in OPS))
    print("FE sub-opcodes with no table entry: %d of 256 (hit in data: see unknown list)" % sum(
        1 for k in range(256) if (0xFE00 | k) not in OPS))
    known = len(OPS)
    used = len(hist)
    print("opcode table entries: %d (main %d + FE %d); distinct opcodes used in data: %d" % (
        known, sum(1 for k in OPS if k <= 0xFF), sum(1 for k in OPS if k > 0xFF), used))
    print("unused-in-data table entries: %s" % " ".join(
        ("FE%02X" % (k & 0xFF) if k > 0xFF else "%02X" % k) for k in sorted(OPS) if k not in hist))
    print("unknown opcodes hit: %s" % (", ".join("%s x%d (e.g. %s)" % (k, v, unk_rooms[k][0]) for k, v in sorted(unk.items())) or "none"))
    errs = collections.Counter(m.split("@")[0].strip() for _, m in fails)
    print("error classes: %s" % dict(errs))
    for room, msg in fails[:args.show]:
        print("  FAIL %s: %s" % (room, msg))
    print("opcode histogram (count):")
    row = []
    for k, v in sorted(hist.items(), key=lambda x: -x[1]):
        nm = OPS[k].name
        row.append("%s %s:%d" % ("FE%02X" % (k & 0xFF) if k > 0xFF else "%02X" % k, nm, v))
    for i in range(0, len(row), 4):
        print("  " + "   ".join("%-34s" % r for r in row[i:i + 4]))


def cmd_dlgstats(args):
    arc = Archive(args.data)
    tot = bad = 0
    ctrl = collections.Counter()
    for room in arc.rooms:
        d = Dialogue(arc.dialogue_blob(room))
        for m in d.msgs:
            tot += 1
            t, ok = Dialogue.render(m[5])
            if not ok:
                bad += 1
            for tok in t.split():
                if tok.startswith("<"):
                    ctrl[tok] += 1
    print("messages %d; not terminated cleanly %d" % (tot, bad))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Chrono Cross field-script disassembler (OpenCross)")
    ap.add_argument("--data", help="path to cdrom.dat (default: ../../../data/cdrom.dat)")
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("disasm"); p.add_argument("room"); p.add_argument("entity", nargs="?"); p.set_defaults(fn=cmd_disasm)
    p = sp.add_parser("dialogue"); p.add_argument("room"); p.set_defaults(fn=cmd_dialogue)
    p = sp.add_parser("stats"); p.add_argument("--show", type=int, default=20); p.set_defaults(fn=cmd_stats)
    p = sp.add_parser("list"); p.set_defaults(fn=cmd_list)
    p = sp.add_parser("optable"); p.add_argument("--counts", action="store_true"); p.set_defaults(fn=cmd_optable)
    p = sp.add_parser("dlgstats"); p.set_defaults(fn=cmd_dlgstats)
    a = ap.parse_args(argv)
    try:
        a.fn(a)
    except (BrokenPipeError, OSError) as ex:
        if getattr(ex, "errno", None) not in (None, 22, 32):
            raise


if __name__ == "__main__":
    main()

// Checks for the model importer on a hand-built Mdl (2 joints, 3 vertices incl. one two-bone vertex, a flat and a
// textured triangle with a negative back-reference pointer, one 2-frame animation) plus the rotation order.
#include <cmath>
#include <cstdio>

#include "import/model.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

bool near(float lhs, float rhs, float eps = 1e-3f) { return std::fabs(lhs - rhs) <= eps; }

void p16(Bytes& out, int value) {
  out.push_back(static_cast<u8>(value & 0xFF));
  out.push_back(static_cast<u8>((value >> 8) & 0xFF));
}
void p32(Bytes& out, u32 value) {
  p16(out, static_cast<int>(value & 0xFFFF));
  p16(out, static_cast<int>(value >> 16));
}
void put32_at(Bytes& out, size_t at, u32 value) {
  for (int i = 0; i < 4; ++i) out[at + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
}

// The hand-built Mdl, section by section.
// S1 (offsets relative to S1): 1 construct at 16; UM at 84, VP at 108, VD at 140. Two 8-byte vertices (one on joint 0, one
// on joint 1), one 16-byte vertex (VO16 = 8 * n8; NV 1, J1 0 W1 2048, J2 1 W2 2048) and two shading entries (0x20 and 0x24).
// UM: a flat triangle (colour, code 0x20, pointers to vertices 0, 1, 2, pad) and a textured triangle (u,v x3, pointers
// stored rotated: ptr[0] -> UV3, ptr[1] -> UV1, ptr[2] -> UV2; UV1 is a back-reference to slot 0 = vertex 0: 4 slots so
// far, so floor(p / 16) = -4 -> p = -64; UV3 -> vertex 2 (unit 8), UV2 -> vertex 1 (unit 16)).
// VP: 8-byte vertices (x y z index) then the 16-byte vertex as two 8-byte records (rec0 relative to joint 1, rec1 to joint 0).
// VD: one s8 x,y,z,pad normal per position record (vertex 0, vertex 1, the 16-byte vertex's rec0 and rec1).
// S2: a 2-joint skeleton (joint 1: parent 0, rotation 1024 about z, translation x 50, bone 1).
// S3: attach point 0 = (1, 2, 3) on joint 1; the 16 shading colours at 0x100 are 0x80 grey.
// S4: cpt{1 anim} + EOF: 2 frames, flags 4 (joint 1 rotation, bit 2); frame 0 is 2 joints x 6 zero s16, frame 1 gives joint
// 1 a rotation delta of +45 degrees (512) about z.
// S5: animation-sequence table with one entry: header (sub-sequence 0 at byte 0x10), then 17 15 01 08 09 00.
Bytes build_mdl() {
  Bytes s1;
  p32(s1, 1);
  p32(s1, 16);
  p32(s1, 0);
  p32(s1, 0);
  const u32 um = 84, vp = 108, vd = 140;
  p32(s1, um);
  p32(s1, vp);
  p32(s1, vd);
  p32(s1, 2);
  p32(s1, 0);
  p16(s1, 1); p16(s1, 0);
  p16(s1, 1); p16(s1, 1);
  p32(s1, 1);
  p32(s1, 16);
  p16(s1, 1); p16(s1, 0); p16(s1, 0); p16(s1, 2048); p16(s1, 1); p16(s1, 2048);
  p32(s1, 2);
  p16(s1, 0x20); p16(s1, 1); p32(s1, 0);
  p16(s1, 0x24); p16(s1, 1); p32(s1, 12);
  CHECK(s1.size() == um);
  s1.push_back(200); s1.push_back(200); s1.push_back(200); s1.push_back(0x20);
  p16(s1, 0); p16(s1, 8); p16(s1, 16); p16(s1, 0);
  for (int uv_byte : {10, 20, 30, 20, 30, 40}) s1.push_back(static_cast<u8>(uv_byte));
  p16(s1, 16);
  p16(s1, -64);
  p16(s1, 16);
  CHECK(s1.size() == vp);
  p16(s1, 10); p16(s1, 20); p16(s1, 30); p16(s1, 0);
  p16(s1, 1); p16(s1, 0); p16(s1, 0); p16(s1, 1);
  p16(s1, 10); p16(s1, 0); p16(s1, 0); p16(s1, 2);
  p16(s1, 50); p16(s1, 10); p16(s1, 0); p16(s1, 2);
  CHECK(s1.size() == vd);
  for (int normal_byte : {0x7F, 0x00, 0x81, 0, 0x00, 0x7F, 0x00, 0, 0x00, 0x00, 0x7F, 0, 1, 1, 1, 0}) s1.push_back(static_cast<u8>(normal_byte));

  Bytes s2;
  p32(s2, 2);
  p32(s2, 0xFFFFFFFFu); p16(s2, 0); p16(s2, 0); p16(s2, 0); p16(s2, 0); p16(s2, -100); p16(s2, 0); p32(s2, 0xFFFFFFFFu);
  p32(s2, 0); p16(s2, 0); p16(s2, 0); p16(s2, 1024); p16(s2, 50); p16(s2, 0); p16(s2, 0); p32(s2, 1u << 16);

  Bytes s3(0x160, 0);
  s3[0] = 1; s3[2] = 2; s3[4] = 3; s3[6] = 1;
  for (size_t i = 0; i < 16; ++i) {
    s3[0x100 + 4 * i] = s3[0x100 + 4 * i + 1] = s3[0x100 + 4 * i + 2] = 0x80;
  }

  Bytes s4;
  p32(s4, 1);
  p32(s4, 12);
  p32(s4, 12 + 46);
  p32(s4, 2);
  p32(s4, 4);
  p32(s4, 4 + 4 + 8);
  p32(s4, 4 + 4 + 8 + 24);
  for (int i = 0; i < 12; ++i) p16(s4, 0);
  p16(s4, 0); p16(s4, 0); p16(s4, 512);
  CHECK(s4.size() == 12 + 46);

  Bytes s5;
  p32(s5, 1);
  p32(s5, 8);
  p16(s5, 0x10);
  for (int i = 0; i < 7; ++i) p16(s5, 0);
  for (int word : {0x17, 0x15, 1, 8, 9, 0}) p16(s5, word);

  Bytes out;
  p32(out, 6);
  const size_t header_size = 0x20;
  size_t pos = header_size;
  u32 section_offsets[5];
  const Bytes* sections[5] = {&s1, &s2, &s3, &s4, &s5};
  for (int i = 0; i < 5; ++i) {
    section_offsets[i] = static_cast<u32>(pos);
    pos += sections[i]->size();
  }
  for (u32 offset : section_offsets) p32(out, offset);
  p32(out, 0);
  p32(out, static_cast<u32>(pos));
  for (const Bytes* section : sections) out.insert(out.end(), section->begin(), section->end());
  return out;
}

// The whole model: skeleton rest pose, the three pool vertices (the two-bone vertex sits at (50, -90, 0) from both of its
// records), the normals kept for the lit path (an 8-byte vertex's own record rotated by its joint, a 16-byte vertex's
// rec0 by J2), the faces (the flat triangle is decoded because its pointer slots feed the back-reference, but only the
// textured one is drawn; without a texture the UVs are normalised by the 128x256 fallback), attach points, shading,
// the animation-sequence entry, the clip (rest + delta) and validate_model; a trailing block after the EOF pointer (as
// HD-mode cdrom2 models have) is ignored; malformed input fails cleanly.
void test_model() {
  const Bytes mdl = build_mdl();
  std::string err;
  auto model = import::load_model(mdl, {}, {}, &err);
  CHECK(model.has_value());
  if (!model) {
    std::printf("  error: %s\n", err.c_str());
    return;
  }
  {
    Bytes padded = mdl;
    padded.resize(padded.size() + 40, 0xAB);
    auto padded_model = import::load_model(padded, {}, {}, &err);
    CHECK(padded_model.has_value() && padded_model->skeleton.joints.size() == 2);
  }
  CHECK(model->skeleton.joints.size() == 2);
  CHECK(model->skeleton.joints[1].parent == 0);
  CHECK(near(model->skeleton.joints[1].rest_rot_deg[2], 90.0f));
  CHECK(near(model->skeleton.joints[1].rest_trans[0], 50.0f));
  CHECK(model->skeleton.joints[0].bone == -1 && model->skeleton.joints[1].bone == 1);
  CHECK(near(model->skeleton.joints[1].rest_quat[2], std::sqrt(0.5f)) && near(model->skeleton.joints[1].rest_quat[3], std::sqrt(0.5f)));

  CHECK(model->meshes.size() == 1);
  const auto& mesh = model->meshes[0];
  CHECK(mesh.pool.size() == 3);
  CHECK(near(mesh.pool[0].bind_pos[0], 10) && near(mesh.pool[0].bind_pos[1], -80) && near(mesh.pool[0].bind_pos[2], 30));
  CHECK(near(mesh.pool[1].bind_pos[0], 50) && near(mesh.pool[1].bind_pos[1], -99));
  CHECK(mesh.pool[2].joint[0] == 0 && mesh.pool[2].joint[1] == 1);
  CHECK(near(mesh.pool[2].weight[0], 0.5f) && near(mesh.pool[2].weight[1], 0.5f));
  CHECK(near(mesh.pool[2].local[0][0], 50) && near(mesh.pool[2].local[1][0], 10));
  CHECK(near(mesh.pool[2].bind_pos[0], 50) && near(mesh.pool[2].bind_pos[1], -90));
  CHECK(mesh.pool[0].normal[0] == 127 && mesh.pool[0].normal[1] == 0 && mesh.pool[0].normal[2] == -127 && mesh.pool[0].normal_joint == 0);
  CHECK(mesh.pool[1].normal[1] == 127);
  CHECK(mesh.pool[2].normal[2] == 127 && mesh.pool[2].normal[0] == 0 && mesh.pool[2].normal_joint == 1);

  CHECK(mesh.indices.size() == 3);
  CHECK(mesh.tri_quad.size() == 1 && mesh.tri_quad[0] == 0);
  CHECK(mesh.trans == 0);
  CHECK(mesh.submeshes.size() == 1);
  CHECK(mesh.materials.size() == 1);
  if (mesh.indices.size() == 3) {
    CHECK(mesh.vertices[mesh.indices[0]].pool == 0 && mesh.vertices[mesh.indices[1]].pool == 1 &&
          mesh.vertices[mesh.indices[2]].pool == 2);
    CHECK(near(mesh.vertices[mesh.indices[0]].uv[0], 10.5f / 128) && near(mesh.vertices[mesh.indices[2]].uv[1], 40.5f / 256));
  }

  CHECK(model->attach.points[0].used && model->attach.points[0].joint == 1 && near(model->attach.points[0].pos[2], 3));
  CHECK(!model->attach.points[1].used);
  CHECK(model->shading[3].r == 0x80);
  CHECK(model->anim_seq.size() == 1 && model->anim_seq[0].size() == 14 && model->anim_seq[0][0] == 0x10 && model->anim_seq[0][8] == 0x17 &&
        model->anim_seq[0][13] == 0);
  CHECK(import::parse_anim_seq_table(Bytes(8, 0)).empty());

  CHECK(model->clips.size() == 1);
  if (!model->clips.empty()) {
    const auto& clip = model->clips[0];
    CHECK(clip.frame_count == 2);
    CHECK(near(clip.duration, 2.0f / assets::kModelAnimFps, 1e-6f));
    CHECK(clip.tracks.size() == 2 && clip.tracks[0].rot.size() == 1 && clip.tracks[1].rot.size() == 2);
    CHECK(clip.tracks[1].trans.size() == 1);
    CHECK(near(clip.tracks[1].rot[0].v[2], 90.0f) && near(clip.tracks[1].rot[1].v[2], 135.0f));
    CHECK(near(clip.tracks[1].rot[1].time, 1.0f / assets::kModelAnimFps, 1e-6f));
    CHECK(near(clip.tracks[1].trans[0].v[0], 50.0f));
  }

  auto info = import::validate_model(mdl, {}, {}, false);
  CHECK(info.ok() && info.joints == 2 && info.vertices == 3 && info.faces == 2 && info.clips == 1);

  Bytes bad_offset = mdl;
  put32_at(bad_offset, 28, static_cast<u32>(bad_offset.size()) + 4);
  CHECK(!import::load_model(bad_offset, {}, {}, &err).has_value() && !err.empty());
  Bytes cut(mdl.begin(), mdl.begin() + 100);
  CHECK(!import::load_model(cut, {}, {}, nullptr).has_value());
  Bytes junk(64, 0xFF);
  CHECK(!import::load_model(junk, {}, {}, nullptr).has_value());
}

// R = Rz * Ry * Rx, x applied first: (90, 90, 0) maps +y to +z (Rx) and then to +x (Ry); (0, 0, 90) maps +x to +y; the
// quaternion of a 90-degree x rotation agrees with the matrix.
// An anim pack with entries for two skeletons: the entry for another joint count keeps its index as an empty clip.
Bytes anim_entry(u32 joints, u32 flag_words) {
  Bytes out;
  p32(out, 1);
  for (u32 i = 0; i < flag_words; ++i) p32(out, 0);
  p32(out, 8 + 4 * flag_words);
  out.resize(out.size() + 12ull * joints, 0);
  return out;
}

Bytes cpt_of(const std::vector<Bytes>& entries) {
  Bytes out;
  p32(out, static_cast<u32>(entries.size()));
  u32 offset = 8 + 4 * static_cast<u32>(entries.size());
  for (const Bytes& entry : entries) {
    p32(out, offset);
    offset += static_cast<u32>(entry.size());
  }
  p32(out, offset);
  for (const Bytes& entry : entries) out.insert(out.end(), entry.begin(), entry.end());
  return out;
}

void test_mixed_skeleton_pack() {
  const Bytes mdl = build_mdl();
  std::string err;
  const Bytes mixed = cpt_of({anim_entry(39, 3), anim_entry(2, 1), anim_entry(39, 3), anim_entry(2, 1)});
  const auto clips = import::load_mdl_clips(mdl, mixed, "tech", &err);
  CHECK(clips.has_value());
  if (clips) {
    CHECK(clips->size() == 4);
    CHECK((*clips)[0].frame_count == 0 && (*clips)[1].frame_count == 1 && (*clips)[2].frame_count == 0 && (*clips)[3].frame_count == 1);
    CHECK((*clips)[1].index == 1 && (*clips)[3].index == 3);
  }
  const Bytes foreign = cpt_of({anim_entry(39, 3)});
  CHECK(!import::load_mdl_clips(mdl, foreign, "tech", &err).has_value());
}

void test_rotation_order() {
  const assets::Mat3 rot_xy = assets::rotation_zyx({90.0f, 90.0f, 0.0f});
  const float rotated_y[3] = {rot_xy[1], rot_xy[4], rot_xy[7]};
  CHECK(near(rotated_y[0], 1) && near(rotated_y[1], 0) && near(rotated_y[2], 0));
  const assets::Mat3 rot_z = assets::rotation_zyx({0.0f, 0.0f, 90.0f});
  CHECK(near(rot_z[0], 0) && near(rot_z[3], 1));
  const auto quat = assets::quat_zyx({90.0f, 0.0f, 0.0f});
  CHECK(near(quat[0], std::sqrt(0.5f)) && near(quat[3], std::sqrt(0.5f)) && near(quat[1], 0) && near(quat[2], 0));
}

// 8bpp TIM with a 2-colour CLUT (index 1 = red); `hw` x `h` halfwords image, every pixel index 1.
Bytes make_tim8(int hw, int h) {
  Bytes tim;
  p32(tim, 0x10);
  p32(tim, 9);
  p32(tim, 12 + 512);
  p32(tim, 0);
  p16(tim, 256);
  p16(tim, 1);
  p16(tim, 0x0000);
  p16(tim, 0x001F);
  tim.resize(tim.size() + 508, 0);
  p32(tim, static_cast<u32>(12 + hw * h * 2));
  p32(tim, 0);
  p16(tim, hw);
  p16(tim, h);
  tim.resize(tim.size() + static_cast<size_t>(hw) * h * 2, 1);
  return tim;
}

// HD textures: an 8 x 16 halfword TIM decodes to 16x16 red opaque pixels; model textures keep STP in alpha (0x0000
// transparent, STP set -> kStpAlpha, clear -> 255); texture_for() prefers the HD set once one exists.
void test_hd() {
  auto img = import::load_hd_tim(make_tim8(8, 16));
  CHECK(img.has_value() && img->width == 16 && img->height == 16 && !img->indexed && img->pixels.size() == 16 * 16 * 4);
  if (img) CHECK(img->pixels[0] == 0xFF && img->pixels[3] == 0xFF);
  CHECK(import::bgr555_to_rgba_stp(0) == 0);
  CHECK(import::bgr555_to_rgba_stp(0x8000) == (static_cast<u32>(import::kStpAlpha) << 24));
  CHECK(import::bgr555_to_rgba_stp(0x801F) == ((static_cast<u32>(import::kStpAlpha) << 24) | 0xFFu));
  CHECK(import::bgr555_to_rgba_stp(0x001F) == (0xFF000000u | 0xFFu));
  {
    Bytes tim_stp = make_tim8(8, 16);
    tim_stp[20 + 2] = 0x1F, tim_stp[20 + 3] = 0x80;
    auto stp = import::load_hd_tim(tim_stp);
    CHECK(stp.has_value() && stp->pixels[0] == 0xFF && stp->pixels[3] == import::kStpAlpha);
  }
  CHECK(!import::load_hd_tim(Bytes(20, 0)).has_value());
  assets::ModelAsset asset;
  asset.textures.resize(1);
  asset.textures[0].width = 4;
  CHECK(!asset.has_hd() && &asset.texture_for(0, true) == &asset.textures[0]);
  asset.hd_textures.resize(1);
  asset.hd_textures[0].width = 16;
  CHECK(asset.has_hd() && &asset.texture_for(0, true) == &asset.hd_textures[0] && &asset.texture_for(0, false) == &asset.textures[0]);
}

// Eye swap: frame 1 / 2 of the eye strip (whose r = g = b = strip x) is pasted over the two eye rects, the left rect from
// strip x 0.. / 64.., the right from 32.. / 96..; texels outside the rects stay; frames 0 and 3 give nothing; a 4x texture scales the
// rects and the strip texel by 4 (nearest), a texture of any other size by its own ratio; a replacement strip maps by its size ratio.
void test_eye_swap() {
  assets::ModelAsset asset;
  asset.main_texture_count = 1;
  asset.textures.resize(2);
  asset.textures[0].width = 128, asset.textures[0].height = 256;
  asset.textures[0].pixels.assign(128 * 256 * 4, 7);
  asset.textures[1].width = 128, asset.textures[1].height = 32;
  asset.textures[1].pixels.resize(128 * 32 * 4);
  for (u32 y = 0; y < 32; ++y)
    for (u32 x = 0; x < 128; ++x)
      for (u32 channel = 0; channel < 4; ++channel)
        asset.textures[1].pixels[(y * 128 + x) * 4 + channel] = static_cast<u8>(x + (channel == 3 ? 100 : 0));
  CHECK(asset.eye_frame_pixels(asset.textures[0].pixels, 128, 256, 1).empty());
  asset.eye.valid = true;
  asset.eye.x = {10, 40};
  asset.eye.y = {20, 50};
  const auto frame1 = asset.eye_frame_pixels(asset.textures[0].pixels, 128, 256, 1);
  CHECK(frame1.size() == asset.textures[0].pixels.size());
  auto px = [&](const std::vector<u8>& pixels, u32 w, u32 x, u32 y) { return pixels[(y * w + x) * 4]; };
  CHECK(px(frame1, 128, 10, 20) == 0 && px(frame1, 128, 41, 51) == 32 + 1);
  CHECK(px(frame1, 128, 9, 20) == 7 && px(frame1, 128, 10, 52) == 7 && px(frame1, 128, 72, 50) == 7);
  const auto frame2 = asset.eye_frame_pixels(asset.textures[0].pixels, 128, 256, 2);
  CHECK(px(frame2, 128, 10, 20) == 64 && px(frame2, 128, 40, 50) == 96);
  CHECK(asset.eye_frame_pixels(asset.textures[0].pixels, 128, 256, 0).empty() && asset.eye_frame_pixels(asset.textures[0].pixels, 128, 256, 3).empty());
  std::vector<u8> big(512 * 1024 * 4, 7);
  const auto big_frame = asset.eye_frame_pixels(big, 512, 1024, 1);
  CHECK(big_frame.size() == big.size() && px(big_frame, 512, 40, 80) == 0 && px(big_frame, 512, 43, 80) == 0 && px(big_frame, 512, 44, 80) == 1 &&
        px(big_frame, 512, 39, 80) == 7);
  CHECK(px(big_frame, 512, 160 + 4 * 31 + 3, 200) == 32 + 31);
  // A replacement of any size: the rects scale by its own ratio (130 / 128 wide).
  const auto odd_frame = asset.eye_frame_pixels(std::vector<u8>(130 * 256 * 4, 7), 130, 256, 1);
  CHECK(odd_frame.size() == 130 * 256 * 4 && px(odd_frame, 130, 10, 20) == 0 && px(odd_frame, 130, 11, 20) == 1 && px(odd_frame, 130, 9, 20) == 7);
  // A replacement strip (2x) maps onto the 1x strip by its size ratio: the same frame as the model's own strip.
  import::Image strip2;
  strip2.width = 256, strip2.height = 64;
  strip2.pixels.resize(256 * 64 * 4);
  for (u32 y = 0; y < 64; ++y)
    for (u32 x = 0; x < 256; ++x)
      for (u32 channel = 0; channel < 4; ++channel)
        strip2.pixels[(y * 256 + x) * 4 + channel] = asset.textures[1].pixels[((y / 2) * 128 + x / 2) * 4 + channel];
  CHECK(asset.eye_frame_pixels(asset.textures[0].pixels, 128, 256, 2, &strip2) == frame2);
  CHECK(asset.eye_frame_pixels(big, 512, 1024, 1, &strip2) == big_frame);
}

// Assembles sections S1..S5 (S6 = 0) into an Mdl container.
Bytes assemble(const std::vector<Bytes>& sections) {
  Bytes out;
  p32(out, 6);
  size_t pos = 0x20;
  for (const Bytes& section : sections) p32(out, static_cast<u32>(pos)), pos += section.size();
  p32(out, 0);
  p32(out, static_cast<u32>(pos));
  for (const Bytes& section : sections) out.insert(out.end(), section.begin(), section.end());
  return out;
}

// mdl_section / mdl_with_mesh_of: a model takes the (longer) mesh section of a donor with the same skeleton while keeping
// its own other sections (the donor's S3 must not leak in); a donor with another skeleton or a junk model is refused.
void test_mesh_splice() {
  const Bytes mdl = build_mdl();
  std::vector<Bytes> sections;
  for (int k = 0; k < 5; ++k) {
    const ByteSpan section = import::mdl_section(mdl, k);
    sections.emplace_back(section.begin(), section.end());
  }
  CHECK(import::mdl_section(mdl, 5).empty());
  Bytes donor_s1 = sections[0];
  donor_s1.resize(donor_s1.size() + 16, 0xEE);
  Bytes donor_s3(sections[2].size(), 0x11);
  const Bytes donor = assemble({donor_s1, sections[1], donor_s3, sections[3], sections[4]});
  const auto out = import::mdl_with_mesh_of(mdl, donor);
  CHECK(out.has_value());
  if (out) {
    CHECK(out->size() == mdl.size() + 16);
    const ByteSpan out_s1 = import::mdl_section(*out, 0);
    CHECK(Bytes(out_s1.begin(), out_s1.end()) == donor_s1);
    for (int k = 1; k < 5; ++k) {
      const ByteSpan section = import::mdl_section(*out, k);
      CHECK(Bytes(section.begin(), section.end()) == sections[static_cast<size_t>(k)]);
    }
    std::string err;
    CHECK(import::load_model(*out, {}, {}, &err).has_value());
  }
  Bytes other_s2 = sections[1];
  other_s2[8] ^= 1;
  CHECK(!import::mdl_with_mesh_of(mdl, assemble({donor_s1, other_s2, sections[2], sections[3], sections[4]})).has_value());
  CHECK(!import::mdl_with_mesh_of(Bytes(8, 0), donor).has_value());
}

}  // namespace

int main() {
  test_mesh_splice();
  test_eye_swap();
  test_hd();
  test_model();
  test_mixed_skeleton_pack();
  test_rotation_order();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_model: ok\n");
  return 0;
}

// FieldWorld: multiplayer presence. What the local party reports (local_presence), and the remote parties of the room (RemoteParties):
// their models through the character-model cache on workers, one tick after each field tick, their meshes and floor shadows drawn with
// the party actors' look, and the name label points. They read the room (walk mesh, lighting, camera) and write nothing of it.
#include "game/field_world.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "render/texture_registry.h"

namespace oc::game {

namespace {

// [Multiplayer] Translucent: the opacity of a remote member and of its shadow.
constexpr float kTranslucentOpacity = 0.5f;
// The label point: this far above attach point 1 (the head), or this high above the feet when the model has no such point.
constexpr float kLabelLift = 64.0f;
constexpr float kLabelHeight = 256.0f;

}  // namespace

// The party leader (slot 0's entity) and what the field can tell of the player: a battle being set up, a script screen (menu, shop,
// name entry, save) requested or open, an event (a movie, or the controls held by a script, a message window, a fade or a room change);
// the pad's own main menu and the pause screen are the host's. The room is reported once the leader stands in it (placed by the room
// load), so the first pose others get is a real spot of this room.
net::LocalPresence FieldWorld::local_presence() const {
  net::LocalPresence presence;
  if (!view_.loaded() || room_lost_) return presence;
  const int lead = party_ent_[0];
  if (!valid_ent(lead) || !ents_[static_cast<size_t>(lead)].placed) return presence;
  presence.room = room_name();
  presence.star = gs_->slot.stars_total();
  for (size_t slot = 0; slot < presence.models.size() && slot < party_chars_.size(); ++slot) {
    const int ent = party_ent_[slot];
    const int shown = valid_ent(ent) ? ents_[static_cast<size_t>(ent)].char_model : -1;
    presence.models[slot] = shown >= 0 ? shown : model_byte(party_chars_[slot]);
  }
  const Actor& actor = ents_[static_cast<size_t>(lead)].actor;
  const ActorPose& pose = actor.current();
  presence.pose.x = static_cast<int>(std::lround(pose.x));
  presence.pose.y = static_cast<int>(std::lround(pose.y));
  presence.pose.z = static_cast<int>(std::lround(pose.z));
  presence.pose.facing = actor.facing();
  presence.pose.moving = actor.moving();
  const int clip = actor.submitted_anim();
  presence.pose.clip = clip >= 0 && clip < kExtraAnimBase ? clip : kClipStandBy;
  if (handoff_.requested()) presence.pose.state = net::PresenceState::Battle;
  else if (menu_state_ != MenuState::None) presence.pose.state = net::PresenceState::Menu;
  else if (movie_state_ != MovieState::None || !controls_free()) presence.pose.state = net::PresenceState::Event;
  return presence;
}

void FieldWorld::set_remote_parties(std::vector<net::RemoteParty> parties, const net::RemoteDrawOptions& options) {
  net_remote_ = std::move(parties);
  remote_options_ = options;
}

std::vector<net::RemoteLabel> FieldWorld::remote_labels() const { return remote_labels_; }

int FieldWorld::remote_drawn() const {
  return static_cast<int>(std::count_if(remote_.parties().begin(), remote_.parties().end(), [](const RemoteParties::Party& party) { return party.visible; }));
}

// A model byte already in the cache (a failed one is cached as null), else the finished worker's (never a load on this thread: a worker
// that failed caches null, one done for an older texture set is started again), else a worker is started for it. A byte the remote side
// asked for first is remembered, so a room change, or the last remote party leaving, can drop it when nothing shows it any more.
std::shared_ptr<ModelHandle> FieldWorld::remote_model(int model_byte, bool& failed) {
  failed = false;
  if (const auto cached = char_models_.find(model_byte); cached != char_models_.end()) {
    failed = !cached->second;
    return cached->second;
  }
  if (const auto job = char_prefetch_.find(model_byte); job != char_prefetch_.end()) {
    if (job->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
    std::optional<PrefetchedCharacter> prefetched = job->second.get();
    char_prefetch_.erase(job);
    if (prefetched && prefetched->generation != render::texture_registry::generation()) {
      prefetch_character_model(model_byte);
      return nullptr;
    }
    std::shared_ptr<ModelHandle> handle;
    if (prefetched) handle = Actor::make_resolved_model(*meshes_, std::move(prefetched->asset), &prefetched->uploads, &prefetched->textures);
    char_models_[model_byte] = handle;
    failed = !handle;
    return handle;
  }
  if (model_byte < 0 || model_byte >= 0x100 || !files_ || model_stem(model_byte).empty()) {
    failed = true;
    return nullptr;
  }
  remote_loaded_.insert(model_byte);
  prefetch_character_model(model_byte);
  return nullptr;
}

// The models only remote parties asked for leave the cache once no remote member wants them and nothing else holds them.
void FieldWorld::drop_remote_models() {
  const std::vector<int> wanted = remote_.wanted_models();
  for (auto it = remote_loaded_.begin(); it != remote_loaded_.end();) {
    const auto cached = char_models_.find(*it);
    if (char_prefetch_.contains(*it)) {
      ++it;
    } else if (cached == char_models_.end()) {
      it = remote_loaded_.erase(it);
    } else if (std::find(wanted.begin(), wanted.end(), *it) == wanted.end() && (!cached->second || cached->second.use_count() == 1)) {
      menu_bodies_.erase(cached->first);
      char_models_.erase(cached);
      it = remote_loaded_.erase(it);
    } else {
      ++it;
    }
  }
}

// The list of this tick is the network's, then the route's. A room change or an art swap places every member again and drops the models
// the room before needed; so does the last remote party leaving.
void FieldWorld::step_remote() {
  if (remote_.empty() && net_remote_.empty() && route_remote_.empty()) {
    if (!remote_loaded_.empty()) drop_remote_models();
    return;
  }
  if (!view_.loaded()) return;
  std::vector<net::RemoteParty> parties = net_remote_;
  parties.insert(parties.end(), route_remote_.begin(), route_remote_.end());
  remote_.set(parties, tick_count_);
  if (!remote_current()) {
    remote_room_ = room_loads_;
    remote_art_ = art_swaps_;
    remote_.reset_room();
    drop_remote_models();
  }
  float target_x = 0, target_z = 0;
  if (const int target = camera_entity(); valid_ent(target)) {
    target_x = ents_[static_cast<size_t>(target)].actor.current().x;
    target_z = ents_[static_cast<size_t>(target)].actor.current().z;
  }
  remote_.tick(tick_count_, view_.walk(), lighting_, [this](int model_byte, bool& failed) { return remote_model(model_byte, failed); }, target_x, target_z,
               remote_options_);
  match_party_scale();
}

// Remote members take the local leader's model scale (the room's scripts size the party, op 82), each by its own model's field size.
void FieldWorld::match_party_scale() {
  const int lead = party_ent_[0];
  if (!valid_ent(lead)) return;
  const Actor& local = ents_[static_cast<size_t>(lead)].actor;
  const float local_size = local.model_handle() ? local.model_handle()->field_scale : 1.0f;
  for (RemoteParties::Party& party : remote_.parties())
    for (RemoteParties::Member& member : party.members) {
      const float size = member.actor.model_handle() ? member.actor.model_handle()->field_scale : 1.0f;
      member.actor.scale = local.scale;
      for (size_t axis = 0; axis < 3; ++axis) member.actor.axis_scale[axis] = local.axis_scale[axis] / local_size * size;
    }
}

// Each member as a party actor is drawn (same pipeline state, shading and art), on the dry path; translucent ones at kTranslucentOpacity.
void FieldWorld::draw_remote_actors(render::MeshPipeline& meshes, const render::Frame& frame, float alpha, const render::Rect& scissor,
                                    const render::Mat4& view_proj, const render::Mat4& view, float water_bias, bool& begun) {
  remote_labels_.clear();
  if (!remote_current()) return;
  std::vector<render::Mat4> palette;
  float scroll_x = 0, scroll_y = 0;
  view_.scroll_at(alpha, scroll_x, scroll_y);
  for (RemoteParties::Party& party : remote_.parties()) {
    if (!party.visible) continue;
    for (RemoteParties::Member& member : party.members) {
      Actor& actor = member.actor;
      if (!member.placed || !actor.loaded()) continue;
      if (!begun) {
        meshes.begin(frame, scissor);
        begun = true;
      }
      actor.use_hd = hd_;
      actor.palette(alpha, palette);
      render::Mat4 model = actor.model_matrix(alpha);
      const MenuBody* menu = dress(actor, palette, model);
      render::MeshPipeline::DrawParams dp;
      dp.view_proj = view_proj;
      dp.view = view;
      dp.model = model;
      dp.palette = palette;
      dp.linear = view_.linear_filter;
      dp.hd = actor.use_hd;
      dp.shading = character_shading_;
      dp.ot_faces = true;
      dp.water_bias = water_bias;
      dp.hidden_meshes = actor.hidden_parts();
      dp.near_meshes = actor.near_parts();
      if (menu) menu_model_parts(menu->body->asset, dp);
      const auto tint = RoomLighting::tint(member.light, std::array<int, 3>{128, 128, 128});
      for (size_t channel = 0; channel < 3; ++channel) dp.tint[channel] = tint[channel];
      dp.opacity = remote_options_.translucent ? kTranslucentOpacity : 1.0f;
      dp.slot_min = slab_min_;
      dp.slot_max = slab_max_;
      meshes.draw(menu ? *menu->body->gpu : *actor.gpu(), dp);
    }
    const Actor& lead = party.members[0].actor;
    if (!remote_options_.labels || !party.members[0].placed) continue;
    float x = 0, y = 0;
    if (label_point(lead, alpha, scroll_x, scroll_y, x, y)) remote_labels_.push_back({party.info.pid, party.info.name, x, y, party.info.pose.state});
  }
}

// The point is the actor's head lifted by kLabelLift, projected into the window's field picture (Letterbox::field of the window, whatever
// target the picture is drawn into), kept when it lies inside the visible columns and rows.
bool FieldWorld::label_point(const Actor& actor, float alpha, float scroll_x, float scroll_y, float& x, float& y) const {
  if (!ctx_ || !actor.loaded()) return false;
  const ActorPose pose = actor.pose(alpha);
  float head[3];
  const float lift = actor.attach_world(1, head) ? actor.current().y - head[1] + kLabelLift : kLabelHeight;
  render::FieldCamera::Projected projected;
  if (!view_.camera().project({pose.x, pose.y - lift, pose.z}, projected)) return false;
  const render::Letterbox lb = render::Letterbox::field(static_cast<float>(ctx_->extent().width), static_cast<float>(ctx_->extent().height));
  const float column = projected.sx + scroll_x, row = projected.sy + scroll_y;
  if (column < lb.x0 || column > lb.x0 + lb.w || row < 0.0f || row > render::kFieldRows) return false;
  x = lb.ox + column * lb.scale;
  y = lb.oy + row * lb.scale_y;
  return true;
}

void FieldWorld::place_local_label(float alpha) {
  local_label_.reset();
  const int lead = party_ent_[0];
  if (!valid_ent(lead) || !ents_[static_cast<size_t>(lead)].placed) return;
  float scroll_x = 0, scroll_y = 0;
  view_.scroll_at(alpha, scroll_x, scroll_y);
  float x = 0, y = 0;
  if (label_point(ents_[static_cast<size_t>(lead)].actor, alpha, scroll_x, scroll_y, x, y)) local_label_ = net::RemoteLabel{{}, {}, x, y, net::PresenceState::Field};
}

int FieldWorld::remote_shadow_grey(const RemoteParties::Member& member) const {
  return remote_options_.translucent ? static_cast<int>(std::lround(static_cast<float>(member.shadow_grey) * kTranslucentOpacity)) : member.shadow_grey;
}

void FieldWorld::remote_shadows(std::vector<ShadowParams>& shadows, float alpha) const {
  if (!remote_current()) return;
  for (const RemoteParties::Party& party : remote_.parties()) {
    if (!party.visible) continue;
    for (const RemoteParties::Member& member : party.members) {
      if (!member.placed || !member.actor.loaded() || member.shadow_grey <= 0) continue;
      const ActorPose pose = member.actor.pose(alpha);
      ShadowParams shadow;
      shadow.x = pose.x;
      shadow.y = pose.y;
      shadow.z = pose.z;
      ActorShadows::floor_normal(view_.walk(), member.actor.triangle(), shadow.normal);
      shadow.grey = remote_shadow_grey(member);
      if (render::FieldCamera::Projected projected;
          view_.camera().project({shadow.x, shadow.y, shadow.z}, projected) && !in_slab(std::floor(std::max(projected.z, 0.0f) / 16.0f)))
        continue;
      if (member.shadow_tile >= 0) {
        shadow.tile_half = member.shadow_half;
        shadow.tile_uv = render::ShadowPipeline::tile_uv(member.shadow_tile);
        shadow.tile_slope_x = member.shadow_slope_x;
        shadow.tile_slope_z = member.shadow_slope_z;
      }
      shadows.push_back(shadow);
    }
  }
}

}  // namespace oc::game

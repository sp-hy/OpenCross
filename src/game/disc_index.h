#pragma once
// The game's two file indexes (cdrom.fnd = disc 1, cdrom2.fnd = disc 2; both members of cdrom.dat). A record is a 0x40 byte NUL
// padded guest path, record i = file index i. The game never opens a file by name: a map jump asks for map record 368 + 3 * room
// id, a movie opcode for record 6033 + 2 + movie number, a battle for record 2679 + scene, and the disc in the drive decides which index answers. Disc-1-only files
// are dummy\dummy.dat on disc 2, and 240 map records name another room (id 459 is bak_a02f on disc 1, fin_06 on disc 2). Field
// script FE 65 / the disc check at every map transition (variable 0x42 = disc - 1) switch it.
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::game {

class DiscIndex {
 public:
  // Record of room id 0 (3 records per room: .bin .prd .ctd), of movie number 0 and of scene 0's battle script.
  static constexpr int kMapBase = 368;
  static constexpr int kMovieBase = 6035;
  static constexpr int kBattleScriptBase = 2679;

  // False if cdrom.fnd is missing.
  bool load(const vfs::Vfs& vfs);
  // `disc2` may be empty (then both discs share disc 1).
  bool from_bytes(ByteSpan disc1, ByteSpan disc2);
  bool loaded() const { return !rec_[0].empty(); }
  // Normalised record (lower case, '/' separators, drive prefix kept); "" for an index outside the file.
  const std::string& record(int disc, int index) const;
  // "map/mapbin/<stem>.bin" record of a room id -> stem ("" when the record is not a map .bin, e.g. dummy.dat).
  std::string room_stem(int disc, int room_id) const;
  // Some map record of that disc names the room file.
  bool has_room(int disc, std::string_view stem) const;
  // Field model of a character model byte (file index 2 * byte + 2 of the field's file ids): "<folder>/<file>" under fieldobj/player
  // (the file need not be named after its folder: 0x3D = neko2/neko), "" when the record names no field player model.
  std::string field_model_stem(int disc, int model_byte) const;
  // Battle script of a scene as a logical path. Several scenes share a file through their record, so the file's number need not be
  // the scene's; a scene with no script record gets the shared template.
  std::string battle_script(int disc, int scene) const;
  // "c:/movie/<name>.stv" record of a movie number -> movie.dat base name ("" = no movie); newop is a01op (language suffixes are the
  // player's: a01op / b28end / final have a per-language file).
  std::string movie_name_of(int disc, int number) const;
  // The D4 / 7E operand (low byte) is NOT the movie number: it indexes the movie parameter table of the field overlay, whose row
  // holds the movie number (+ play range, mode, volume). -> movie.dat base name, "" when the index has no row / no movie.
  std::string movie_name(int disc, int op) const;
  struct MovieRow {
    // Frame range (start 0 = from the beginning, end = frame count); mode 3 / 4 = the staff-roll layout.
    u16 start, end;
    u8 mode, number, volume;
  };
  static const MovieRow* movie_row(int disc, int op);
  // Frame range the game plays of that movie: disc 1 rows 20 / 21 / 23 / 22 and disc 2 row 12 play a part; every other row plays
  // the whole file (start = end = 0). Frames are Bink's 1-based numbers.
  static void movie_range(int disc, int op, u32& start, u32& end);

 private:
  std::vector<std::string> rec_[2];
};

}  // namespace oc::game

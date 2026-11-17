// Optical drive access through the Windows storage and CD-ROM device controls (also served by Wine / Proton). Raw sectors come from
// a SCSI READ CD pass-through that accepts any sector type, else from the CD-ROM raw-read control in XA form-2 or mode-2 track mode;
// the first method whose probe returns a sector with a valid sync and address is kept. Cooked sectors are plain reads of the
// volume device at 2048-byte offsets. Other systems report no drives.
#include "platform/optical.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <ntddscsi.h>
#endif

namespace oc::platform {

const char* raw_read_method_name(RawReadMethod method) {
  switch (method) {
    case RawReadMethod::Cooked: return "cooked 2048 only";
    case RawReadMethod::ReadCd: return "raw via READ CD";
    case RawReadMethod::RawXaForm2: return "raw via raw read (XA form 2)";
    case RawReadMethod::RawYellowMode2: return "raw via raw read (mode 2)";
  }
  return "cooked 2048 only";
}

#ifdef _WIN32

namespace {

constexpr size_t kRawSize = 2352;
constexpr size_t kCookedSize = 2048;
constexpr u32 kLeadInFrames = 150;
// Sectors per device request: 16 raw sectors stay under the 64 KiB a pass-through transfer is always allowed.
constexpr u32 kChunk = 16;
constexpr size_t kBufferAlign = 4096;
// How far back from the reported end the last readable sector is searched for.
constexpr u32 kEndSearch = 16;

u32 from_bcd(u8 value) { return (value >> 4) * 10u + (value & 0x0F); }

// A raw sector whose sync pattern is intact and whose header address is `lba`.
bool raw_sector_ok(const u8* sector, u32 lba) {
  if (sector[0] != 0 || sector[11] != 0) return false;
  for (int i = 1; i < 11; ++i)
    if (sector[i] != 0xFF) return false;
  const u32 frames = (from_bcd(sector[12]) * 60 + from_bcd(sector[13])) * 75 + from_bcd(sector[14]);
  return frames == lba + kLeadInFrames;
}

}  // namespace

struct OpticalDrive::Handle {
  HANDLE file = INVALID_HANDLE_VALUE;
  ~Handle() {
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  }
};

namespace {

// Suppresses the system's "insert a disc" dialog while a drive is probed.
class QuietErrors {
 public:
  QuietErrors() { SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_); }
  ~QuietErrors() { SetThreadErrorMode(previous_, nullptr); }
  QuietErrors(const QuietErrors&) = delete;
  QuietErrors& operator=(const QuietErrors&) = delete;

 private:
  DWORD previous_ = 0;
};

std::string utf8(const wchar_t* text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring device_path(wchar_t letter) { return std::wstring(L"\\\\.\\") + letter + L":"; }

HANDLE open_device(wchar_t letter, DWORD access) {
  return CreateFileW(device_path(letter).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

std::optional<u32> change_count(HANDLE file) {
  ULONG count = 0;
  DWORD returned = 0;
  if (DeviceIoControl(file, IOCTL_STORAGE_CHECK_VERIFY2, nullptr, 0, &count, sizeof(count), &returned, nullptr))
    return returned >= sizeof(count) ? count : 0u;
  if (DeviceIoControl(file, IOCTL_STORAGE_CHECK_VERIFY, nullptr, 0, &count, sizeof(count), &returned, nullptr))
    return returned >= sizeof(count) ? count : 0u;
  return std::nullopt;
}

std::string trimmed(std::string text) {
  while (!text.empty() && text.back() == ' ') text.pop_back();
  const size_t first = text.find_first_not_of(' ');
  return first == std::string::npos ? std::string() : text.substr(first);
}

std::string product_of(HANDLE file) {
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceProperty;
  query.QueryType = PropertyStandardQuery;
  u8 buffer[1024] = {};
  DWORD returned = 0;
  if (!DeviceIoControl(file, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &returned, nullptr)) return {};
  const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);
  const auto field = [&](DWORD offset) {
    if (offset == 0 || offset >= returned) return std::string();
    const char* text = reinterpret_cast<const char*>(buffer + offset);
    return trimmed(std::string(text, strnlen(text, returned - offset)));
  };
  const std::string vendor = field(descriptor->VendorIdOffset);
  const std::string product = field(descriptor->ProductIdOffset);
  return vendor.empty() ? product : product.empty() ? vendor : vendor + " " + product;
}

std::optional<wchar_t> drive_letter(const std::string& drive) {
  if (drive.empty()) return std::nullopt;
  const char letter = static_cast<char>(drive[0] & ~0x20);
  if (letter < 'A' || letter > 'Z') return std::nullopt;
  const std::string rest = drive.substr(1);
  if (!rest.empty() && rest != ":" && rest != ":\\" && rest != ":/") return std::nullopt;
  return static_cast<wchar_t>(letter);
}

u8* aligned(std::vector<u8>& buffer, size_t size) {
  if (buffer.size() < size + kBufferAlign) buffer.resize(size + kBufferAlign);
  const auto address = reinterpret_cast<uintptr_t>(buffer.data());
  return buffer.data() + ((kBufferAlign - address % kBufferAlign) % kBufferAlign);
}

}  // namespace

std::vector<OpticalDriveInfo> list_optical_drives() {
  const QuietErrors quiet;
  std::vector<OpticalDriveInfo> out;
  const DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (!(mask & (1u << i))) continue;
    const wchar_t letter = static_cast<wchar_t>(L'A' + i);
    const wchar_t root[] = {letter, L':', L'\\', 0};
    if (GetDriveTypeW(root) != DRIVE_CDROM) continue;
    OpticalDriveInfo info;
    info.drive = std::string(1, static_cast<char>('A' + i)) + ":";
    const HANDLE file = open_device(letter, 0);
    if (file != INVALID_HANDLE_VALUE) {
      info.product = product_of(file);
      info.media = change_count(file).has_value();
      CloseHandle(file);
    }
    wchar_t label[MAX_PATH + 1] = {};
    if (info.media && GetVolumeInformationW(root, label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0)) info.label = utf8(label);
    out.push_back(std::move(info));
  }
  return out;
}

std::unique_ptr<OpticalDrive> OpticalDrive::open(const std::string& drive, std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return std::unique_ptr<OpticalDrive>();
  };
  const std::optional<wchar_t> letter = drive_letter(drive);
  if (!letter) return fail("not a drive name: " + drive);
  const std::string name = std::string(1, static_cast<char>(*letter)) + ":";
  const wchar_t root[] = {*letter, L':', L'\\', 0};
  if (GetDriveTypeW(root) != DRIVE_CDROM) return fail(name + " is not an optical drive");
  const QuietErrors quiet;
  auto handle = std::make_unique<Handle>();
  handle->file = open_device(*letter, GENERIC_READ | GENERIC_WRITE);
  if (handle->file == INVALID_HANDLE_VALUE) handle->file = open_device(*letter, GENERIC_READ);
  if (handle->file == INVALID_HANDLE_VALUE) return fail("cannot open " + name + " (error " + std::to_string(GetLastError()) + ")");
  if (!change_count(handle->file)) return fail("no disc in " + name);
  DWORD returned = 0;
  DeviceIoControl(handle->file, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &returned, nullptr);
  std::unique_ptr<OpticalDrive> out(new OpticalDrive(name, std::move(handle)));
  out->product_ = product_of(out->handle_->file);
  if (!out->read_toc() || out->sector_count_ == 0) return fail("cannot read the table of contents of " + name);
  u32 probe = 0;
  for (const OpticalTrack& track : out->tracks_) {
    if (!track.data) continue;
    probe = track.start_lba + 16 < out->sector_count_ ? track.start_lba + 16 : track.start_lba;
    break;
  }
  u8* sector = aligned(out->scratch_, kRawSize);
  for (const RawReadMethod method : {RawReadMethod::ReadCd, RawReadMethod::RawXaForm2, RawReadMethod::RawYellowMode2}) {
    if (out->raw_with(method, probe, 1, sector) && raw_sector_ok(sector, probe)) {
      out->raw_method_ = method;
      break;
    }
  }
  // The table of contents and the device length can disagree by a sector: the disc ends after the last sector that reads.
  const u32 reported = out->sector_count_;
  while (out->sector_count_ > 0 && reported - out->sector_count_ < kEndSearch) {
    const u32 last = out->sector_count_ - 1;
    if (out->raw_method_ == RawReadMethod::Cooked ? out->cooked_with(last, 1, sector) : out->raw_with(out->raw_method_, last, 1, sector)) break;
    --out->sector_count_;
  }
  if (reported - out->sector_count_ >= kEndSearch) out->sector_count_ = reported;
  return out;
}

OpticalDrive::~OpticalDrive() = default;

bool OpticalDrive::read_toc() {
  CDROM_TOC toc{};
  DWORD returned = 0;
  u32 lead_out = 0;
  if (DeviceIoControl(handle_->file, IOCTL_CDROM_READ_TOC, nullptr, 0, &toc, sizeof(toc), &returned, nullptr) && toc.LastTrack >= toc.FirstTrack) {
    const int entries = toc.LastTrack - toc.FirstTrack + 1;
    for (int i = 0; i <= entries && i < MAXIMUM_NUMBER_TRACKS; ++i) {
      const TRACK_DATA& track = toc.TrackData[i];
      const u32 frames = (track.Address[1] * 60u + track.Address[2]) * 75u + track.Address[3];
      const u32 lba = frames >= kLeadInFrames ? frames - kLeadInFrames : 0;
      if (i == entries) {
        lead_out = lba;
        break;
      }
      tracks_.push_back({track.TrackNumber, lba, (track.Control & 0x04) != 0});
    }
  }
  GET_LENGTH_INFORMATION length{};
  u32 length_sectors = 0;
  if (DeviceIoControl(handle_->file, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length, sizeof(length), &returned, nullptr))
    length_sectors = static_cast<u32>(length.Length.QuadPart / kCookedSize);
  sector_count_ = std::max(lead_out, length_sectors);
  if (tracks_.empty()) tracks_.push_back({1, 0, true});
  return sector_count_ > 0;
}

bool OpticalDrive::raw_with(RawReadMethod method, u32 lba, u32 count, u8* out) {
  const DWORD bytes = static_cast<DWORD>(count * kRawSize);
  DWORD returned = 0;
  switch (method) {
    case RawReadMethod::Cooked: return false;
    case RawReadMethod::ReadCd: {
      struct {
        SCSI_PASS_THROUGH_DIRECT pass;
        ULONG filler;
        UCHAR sense[32];
      } request{};
      request.pass.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
      request.pass.CdbLength = 12;
      request.pass.DataIn = SCSI_IOCTL_DATA_IN;
      request.pass.DataTransferLength = bytes;
      request.pass.TimeOutValue = 30;
      request.pass.DataBuffer = out;
      request.pass.SenseInfoLength = sizeof(request.sense);
      request.pass.SenseInfoOffset = static_cast<ULONG>(offsetof(decltype(request), sense));
      UCHAR* cdb = request.pass.Cdb;
      cdb[0] = 0xBE;
      cdb[2] = static_cast<UCHAR>(lba >> 24);
      cdb[3] = static_cast<UCHAR>(lba >> 16);
      cdb[4] = static_cast<UCHAR>(lba >> 8);
      cdb[5] = static_cast<UCHAR>(lba);
      cdb[6] = static_cast<UCHAR>(count >> 16);
      cdb[7] = static_cast<UCHAR>(count >> 8);
      cdb[8] = static_cast<UCHAR>(count);
      // Sync, all headers, user data, EDC/ECC: the whole 2352 bytes.
      cdb[9] = 0xF8;
      return DeviceIoControl(handle_->file, IOCTL_SCSI_PASS_THROUGH_DIRECT, &request, sizeof(request), &request, sizeof(request), &returned, nullptr) &&
             request.pass.ScsiStatus == 0 && request.pass.DataTransferLength == bytes;
    }
    case RawReadMethod::RawXaForm2:
    case RawReadMethod::RawYellowMode2: {
      RAW_READ_INFO info{};
      info.DiskOffset.QuadPart = static_cast<LONGLONG>(lba) * kCookedSize;
      info.SectorCount = count;
      info.TrackMode = method == RawReadMethod::RawXaForm2 ? XAForm2 : YellowMode2;
      return DeviceIoControl(handle_->file, IOCTL_CDROM_RAW_READ, &info, sizeof(info), out, bytes, &returned, nullptr) && returned == bytes;
    }
  }
  return false;
}

bool OpticalDrive::read_raw(u32 lba, u32 count, u8* out) {
  const std::lock_guard lock(mutex_);
  if (raw_method_ == RawReadMethod::Cooked || lba > sector_count_ || count > sector_count_ - lba) return false;
  const QuietErrors quiet;
  while (count > 0) {
    const u32 run = std::min(count, kChunk);
    u8* buffer = aligned(scratch_, run * kRawSize);
    if (!raw_with(raw_method_, lba, run, buffer)) return false;
    std::memcpy(out, buffer, run * kRawSize);
    out += run * kRawSize;
    lba += run;
    count -= run;
  }
  return true;
}

bool OpticalDrive::cooked_with(u32 lba, u32 count, u8* out) {
  const DWORD bytes = static_cast<DWORD>(count * kCookedSize);
  LARGE_INTEGER at{};
  at.QuadPart = static_cast<LONGLONG>(lba) * kCookedSize;
  DWORD returned = 0;
  return SetFilePointerEx(handle_->file, at, nullptr, FILE_BEGIN) && ReadFile(handle_->file, out, bytes, &returned, nullptr) && returned == bytes;
}

bool OpticalDrive::read_cooked(u32 lba, u32 count, u8* out) {
  const std::lock_guard lock(mutex_);
  if (lba > sector_count_ || count > sector_count_ - lba) return false;
  const QuietErrors quiet;
  while (count > 0) {
    const u32 run = std::min(count, kChunk);
    u8* buffer = aligned(scratch_, run * kCookedSize);
    if (!cooked_with(lba, run, buffer)) return false;
    std::memcpy(out, buffer, run * kCookedSize);
    out += run * kCookedSize;
    lba += run;
    count -= run;
  }
  return true;
}

std::optional<u32> OpticalDrive::media_generation() {
  const std::lock_guard lock(mutex_);
  const QuietErrors quiet;
  return change_count(handle_->file);
}

#else

struct OpticalDrive::Handle {};

std::vector<OpticalDriveInfo> list_optical_drives() { return {}; }

std::unique_ptr<OpticalDrive> OpticalDrive::open(const std::string& drive, std::string* error) {
  if (error) *error = "optical drives are not supported on this system: " + drive;
  return nullptr;
}

OpticalDrive::~OpticalDrive() = default;

bool OpticalDrive::read_toc() { return false; }

bool OpticalDrive::raw_with(RawReadMethod, u32, u32, u8*) { return false; }

bool OpticalDrive::cooked_with(u32, u32, u8*) { return false; }

bool OpticalDrive::read_raw(u32, u32, u8*) { return false; }

bool OpticalDrive::read_cooked(u32, u32, u8*) { return false; }

std::optional<u32> OpticalDrive::media_generation() { return std::nullopt; }

#endif

OpticalDrive::OpticalDrive(std::string drive, std::unique_ptr<Handle> handle) : drive_(std::move(drive)), handle_(std::move(handle)) {}

RawReadMethod OpticalDrive::raw_method() const {
  const std::lock_guard lock(mutex_);
  return raw_method_;
}

}  // namespace oc::platform

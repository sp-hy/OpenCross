// Runs in the launcher process before the launcher window loads. This is an attribute on its own assembly: the proxy DLL asks for the
// assembly's attributes, which constructs it, because that is a call both the CLR and Mono place in the same COM slot, where the
// _Assembly::CreateInstance slot differs. The launcher builds its whole menu from
// one table (ui.csv: one row per picture, with position, size, action and program) and art files, all read through a single private
// method, so this redirects that method and serves an edited table and art: the launcher's own code then draws, focuses, clicks and
// reloads the sixth button like the other five, and its "GAME" action starts opencross.exe. Nothing is written to disk.
//
// The art is made from the player's own launcher.dat when the launcher asks for it:
//   the button: EXIT's plate with its text painted out, OPENCROSS assembled from letters of the other buttons' text (P is built from R's
//               bowl and stem), redrawn with a shadow;
//   the background: BASE0 has each button's drop shadow baked in. The bottom button sits on plain white, where the picture IS the shadow,
//               so its pixels give a mask that removes all five shadows (divide) and draws six (multiply).
// Any problem leaves the stock launcher; so does a missing opencross.exe.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

[assembly: OpenCross.LauncherPatch]

namespace OpenCross {

[AttributeUsage(AttributeTargets.Assembly)]
public class LauncherPatch : Attribute {
  private const string kEngineExe = "opencross.exe";
  private const string kNewButton = "GAME00";

  // The column of buttons: 280 x 54 each, 4 pixels apart, moved down from y 288 / 62 apart to make room for the first one.
  private const int kButtonX = 857;
  private const int kButtonWidth = 280;
  private const int kButtonHeight = 54;
  private const int kTop = 272;
  private const int kPitch = 58;
  private const int kOldTop = 288;
  private const int kOldPitch = 62;
  private const int kOldCount = 5;

  // The area around one button that its baked shadow can reach.
  private const int kShadowLeft = 848;
  private const int kShadowWidth = 300;
  private const int kShadowAbove = 5;
  private const int kShadowHeight = 72;

  // Below this red level a pixel of the blue plate has no text in it.
  private const int kTextRedFloor = 112;
  private const int kTextLeft = 6;
  private const int kTextRight = 274;
  private const int kTextTop = 14;
  private const int kTextBottom = 46;
  private const int kLetterGap = 3;

  private static readonly Dictionary<string, byte[]> served = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
  private static byte[] buttonOff;
  private static byte[] buttonOn;

  public LauncherPatch() {
    try {
      if (!File.Exists(Path.Combine(Directory.GetCurrentDirectory(), kEngineExe))) return;
      buttonOff = MakeButton("MOUSEOFF");
      buttonOn = MakeButton("MOUSEON");
      Type utility = Assembly.GetEntryAssembly().GetType("Launcher.Utility");
      MethodInfo reader = utility.GetMethod("ReadNormalFile", BindingFlags.NonPublic | BindingFlags.Static);
      MethodInfo replacement = typeof(LauncherPatch).GetMethod("ReadFile", BindingFlags.NonPublic | BindingFlags.Static);
      Redirect(reader, replacement);
    } catch (Exception) {
    }
  }

  // Null means "no such loose file": the launcher then looks in launcher.dat itself.
  private static byte[] ReadFile(string fileName) {
    byte[] data;
    if (served.TryGetValue(fileName, out data)) return data;
    data = Patched(fileName);
    if (data == null) data = ReadLoose(fileName);
    return data;
  }

  private static byte[] Patched(string fileName) {
    if (fileName.EndsWith("\\" + kNewButton + "_MOUSEOFF.png", StringComparison.OrdinalIgnoreCase)) return buttonOff;
    if (fileName.EndsWith("\\" + kNewButton + "_MOUSEON.png", StringComparison.OrdinalIgnoreCase)) return buttonOn;
    byte[] data = null;
    try {
      if (fileName.EndsWith("\\ui.csv", StringComparison.OrdinalIgnoreCase)) {
        data = Encoding.UTF8.GetBytes(EditTable(Encoding.UTF8.GetString(ReadOriginal(fileName))));
      } else if (fileName.EndsWith("\\BASE0.png", StringComparison.OrdinalIgnoreCase)) {
        data = MoveShadows(ReadOriginal(fileName));
      }
    } catch (Exception) {
      data = null;
    }
    if (data != null) served[fileName] = data;
    return data;
  }

  private static byte[] ReadLoose(string fileName) {
    try {
      using (FileStream stream = new FileStream(fileName, FileMode.Open, FileAccess.Read, FileShare.Read)) {
        byte[] data = new byte[stream.Length];
        stream.Read(data, 0, data.Length);
        return data;
      }
    } catch (Exception) {
      return null;
    }
  }

  // The file as the launcher would have found it: loose, else in the archive.
  private static byte[] ReadOriginal(string fileName) {
    byte[] data = ReadLoose(fileName);
    if (data != null) return data;
    const string kRoot = "data\\launcher\\";
    string entryName = fileName.Substring(fileName.IndexOf(kRoot, StringComparison.OrdinalIgnoreCase) + kRoot.Length).Replace('\\', '/');
    using (ZipArchive archive = ZipFile.OpenRead("data\\launcher.dat"))
    using (Stream stream = archive.GetEntry(entryName).Open())
    using (MemoryStream memory = new MemoryStream()) {
      stream.CopyTo(memory);
      return memory.ToArray();
    }
  }

  private static string EditTable(string table) {
    StringBuilder edited = new StringBuilder();
    bool offRowAdded = false;
    edited.Append(TableRow("MOUSEON", "GAME", kEngineExe));
    foreach (string line in table.Replace("\r", "").Split('\n')) {
      if (line.Length == 0) continue;
      string[] fields = line.Split(',');
      int buttonIndex;
      if (fields[0].StartsWith("GAME") && int.TryParse(fields[0].Substring(4, 2), out buttonIndex)) {
        if (!offRowAdded && fields[0].EndsWith("_MOUSEOFF")) {
          edited.Append(TableRow("MOUSEOFF", "NON", ""));
          offRowAdded = true;
        }
        fields[2] = (kTop + buttonIndex * kPitch).ToString();
      }
      edited.Append(string.Join(",", fields)).Append("\r\n");
    }
    return edited.ToString();
  }

  private static string TableRow(string state, string action, string program) {
    return string.Format("{0}_{1},{2},{3},{4},{5},0,0,0,0,{6},{7}\r\n", kNewButton, state, kButtonX, kTop, kButtonWidth, kButtonHeight, action, program);
  }

  // Pixels of a picture as RGB bytes with their alpha, in reading order.
  private sealed class Raster {
    public readonly int width;
    public readonly int height;
    public readonly byte[] bgra;

    public Raster(int width, int height) {
      this.width = width;
      this.height = height;
      bgra = new byte[width * height * 4];
    }

    public Raster(byte[] encoded) {
      using (MemoryStream stream = new MemoryStream(encoded))
      using (Bitmap bitmap = new Bitmap(stream)) {
        width = bitmap.Width;
        height = bitmap.Height;
        bgra = new byte[width * height * 4];
        BitmapData locked = bitmap.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        for (int y = 0; y < height; ++y) Marshal.Copy(locked.Scan0 + y * locked.Stride, bgra, y * width * 4, width * 4);
        bitmap.UnlockBits(locked);
      }
    }

    public int Offset(int x, int y) {
      return (y * width + x) * 4;
    }

    public byte Red(int x, int y) {
      return bgra[Offset(x, y) + 2];
    }

    // The launcher tells art from the background by pixel format, so the background stays 24-bit.
    public byte[] Encode(PixelFormat format) {
      using (Bitmap bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb)) {
        BitmapData locked = bitmap.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        for (int y = 0; y < height; ++y) Marshal.Copy(bgra, y * width * 4, locked.Scan0 + y * locked.Stride, width * 4);
        bitmap.UnlockBits(locked);
        using (Bitmap converted = bitmap.Clone(new Rectangle(0, 0, width, height), format))
        using (MemoryStream stream = new MemoryStream()) {
          converted.Save(stream, ImageFormat.Png);
          return stream.ToArray();
        }
      }
    }
  }

  // Multiplies (or divides) one button's shadow into the picture.
  private static void ApplyShadow(Raster picture, float[] mask, int buttonTop, bool remove) {
    int top = buttonTop - kShadowAbove;
    for (int y = 0; y < kShadowHeight; ++y) {
      for (int x = 0; x < kShadowWidth; ++x) {
        float shade = mask[y * kShadowWidth + x];
        if (shade >= 1.0f) continue;
        int offset = picture.Offset(kShadowLeft + x, top + y);
        for (int channel = 0; channel < 3; ++channel) {
          float value = picture.bgra[offset + channel] * (remove ? 1.0f / shade : shade);
          picture.bgra[offset + channel] = (byte)Math.Min(255.0f, value + 0.5f);
        }
      }
    }
  }

  private static float[] ShadowMask(Raster picture) {
    int bottomButton = kOldTop + (kOldCount - 1) * kOldPitch;
    float[] mask = new float[kShadowWidth * kShadowHeight];
    // The white strip starts 3 rows above the bottom button; the two rows above it are water, so they fade the first white row out at
    // the rate the shadow's left edge does.
    int firstWhite = 2;
    for (int y = firstWhite; y < kShadowHeight; ++y)
      for (int x = 0; x < kShadowWidth; ++x)
        mask[y * kShadowWidth + x] = picture.Red(kShadowLeft + x, bottomButton - kShadowAbove + y) / 255.0f;
    for (int x = 0; x < kShadowWidth; ++x) {
      float edge = 1.0f - mask[firstWhite * kShadowWidth + x];
      mask[kShadowWidth + x] = 1.0f - edge * 0.43f;
      mask[x] = 1.0f - edge * 0.07f;
    }
    return mask;
  }

  private static byte[] MoveShadows(byte[] encoded) {
    Raster picture = new Raster(encoded);
    float[] mask = ShadowMask(picture);
    for (int i = 0; i < kOldCount; ++i) ApplyShadow(picture, mask, kOldTop + i * kOldPitch, true);
    for (int i = 0; i <= kOldCount; ++i) ApplyShadow(picture, mask, kTop + i * kPitch, false);
    return picture.Encode(PixelFormat.Format24bppRgb);
  }

  // A letter of baked button text: how much white each pixel of its box holds.
  private sealed class Letter {
    public int width;
    public int height;
    public float[] cover;
  }

  // The word is the same in every language, so it is always made from the English art.
  private static Raster ReadEnglish(string picture) {
    return new Raster(ReadOriginal("data\\launcher\\Resources\\EN\\Image\\" + picture + ".png"));
  }

  private static float TextCover(Raster button, int x, int y) {
    return Math.Max(0.0f, Math.Min(1.0f, (button.Red(x, y) - kTextRedFloor) / (float)(255 - kTextRedFloor)));
  }

  // Cuts a line of text into letters at the columns with no white in them.
  private static List<Letter> CutLetters(Raster button) {
    List<Letter> letters = new List<Letter>();
    int start = -1;
    for (int x = kTextLeft; x <= kTextRight; ++x) {
      bool ink = false;
      if (x < kTextRight)
        for (int y = kTextTop; y < kTextBottom; ++y) ink |= TextCover(button, x, y) > 0.2f;
      if (ink && start < 0) start = x;
      if (ink || start < 0) continue;
      Letter letter = new Letter();
      letter.width = x - start;
      letter.height = kTextBottom - kTextTop;
      letter.cover = new float[letter.width * letter.height];
      for (int y = 0; y < letter.height; ++y)
        for (int i = 0; i < letter.width; ++i) letter.cover[y * letter.width + i] = TextCover(button, start + i, kTextTop + y);
      letters.Add(letter);
      start = -1;
    }
    return letters;
  }

  // P from R: R's bowl is short and runs into the leg, so the top of the bowl is mirrored below itself to close a taller one, and under
  // that only the stem stays.
  private static Letter MakeP(Letter letter) {
    const int kBowlPercent = 57;
    Letter result = new Letter();
    result.width = letter.width;
    result.height = letter.height;
    result.cover = (float[])letter.cover.Clone();
    int lastRow = -1;
    int firstRow = -1;
    for (int y = 0; y < letter.height; ++y) {
      bool ink = false;
      for (int i = 0; i < letter.width; ++i) ink |= letter.cover[y * letter.width + i] > 0.5f;
      if (ink && firstRow < 0) firstRow = y;
      if (ink) lastRow = y;
    }
    int stemEnd = 0;
    while (stemEnd < letter.width && letter.cover[lastRow * letter.width + stemEnd] < 0.5f) ++stemEnd;
    while (stemEnd < letter.width && letter.cover[lastRow * letter.width + stemEnd] >= 0.5f) ++stemEnd;
    int bowlEnd = firstRow + (lastRow - firstRow) * kBowlPercent / 100;
    for (int y = (firstRow + bowlEnd + 1) / 2; y <= bowlEnd; ++y)
      for (int i = 0; i < letter.width; ++i) result.cover[y * letter.width + i] = letter.cover[(firstRow + bowlEnd - y) * letter.width + i];
    for (int y = bowlEnd + 1; y < letter.height; ++y)
      for (int i = stemEnd + 1; i < letter.width; ++i) result.cover[y * letter.width + i] = 0.0f;
    return result;
  }

  private static byte[] MakeButton(string state) {
    List<Letter> chronoCross = CutLetters(ReadEnglish("GAME01_MOUSEOFF"));
    List<Letter> credits = CutLetters(ReadEnglish("GAME04_MOUSEOFF"));
    if (chronoCross.Count != 11 || credits.Count != 7) throw new InvalidDataException("unexpected button art");
    Letter letterC = chronoCross[0], letterR = chronoCross[2], letterO = chronoCross[3], letterN = chronoCross[4], letterS = chronoCross[9];
    Letter letterE = credits[2];
    Letter[] word = { letterO, MakeP(letterR), letterE, letterN, letterC, letterR, letterO, letterS, letterS };

    Raster plate = ReadEnglish("GAME05_" + state);
    PaintOutText(plate, ReadEnglish("GAME05_MOUSEOFF"));

    int textWidth = (word.Length - 1) * kLetterGap;
    foreach (Letter letter in word) textWidth += letter.width;
    float[] cover = new float[plate.width * plate.height];
    int left = (plate.width - textWidth) / 2;
    foreach (Letter letter in word) {
      for (int y = 0; y < letter.height; ++y)
        for (int i = 0; i < letter.width; ++i) {
          int index = (kTextTop + y) * plate.width + left + i;
          cover[index] = Math.Max(cover[index], letter.cover[y * letter.width + i]);
        }
      left += letter.width + kLetterGap;
    }
    DrawText(plate, cover);
    return plate.Encode(PixelFormat.Format32bppArgb);
  }

  // Grows the text area by the reach of its shadow, then fills it inwards from its edge with the average of the known neighbours.
  private static void PaintOutText(Raster plate, Raster textSource) {
    const int kShadowReach = 4;
    bool[] unknown = new bool[plate.width * plate.height];
    for (int y = kTextTop; y < kTextBottom; ++y)
      for (int x = kTextLeft; x < kTextRight; ++x) {
        if (TextCover(textSource, x, y) <= 0.05f) continue;
        for (int j = -kShadowReach; j <= kShadowReach; ++j)
          for (int i = -kShadowReach; i <= kShadowReach; ++i) unknown[(y + j) * plate.width + x + i] = true;
      }
    bool filled = true;
    while (filled) {
      filled = false;
      bool[] next = (bool[])unknown.Clone();
      for (int y = 1; y < plate.height - 1; ++y)
        for (int x = 1; x < plate.width - 1; ++x) {
          if (!unknown[y * plate.width + x]) continue;
          int[] sum = new int[3];
          int known = 0;
          for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
              if (unknown[(y + j) * plate.width + x + i]) continue;
              int neighbour = plate.Offset(x + i, y + j);
              for (int channel = 0; channel < 3; ++channel) sum[channel] += plate.bgra[neighbour + channel];
              ++known;
            }
          if (known == 0) continue;
          int offset = plate.Offset(x, y);
          for (int channel = 0; channel < 3; ++channel) plate.bgra[offset + channel] = (byte)(sum[channel] / known);
          next[y * plate.width + x] = false;
          filled = true;
        }
      unknown = next;
    }
  }

  // White text over a soft dark-blue shadow one pixel down and right, as on the baked buttons.
  private static void DrawText(Raster plate, float[] cover) {
    const float kShadowStrength = 0.8f;
    for (int y = 2; y < plate.height - 2; ++y) {
      for (int x = 2; x < plate.width - 2; ++x) {
        float shadow = 0.0f;
        for (int j = -1; j <= 1; ++j)
          for (int i = -1; i <= 1; ++i) shadow += cover[(y - 1 + j) * plate.width + x - 1 + i];
        shadow = Math.Min(1.0f, shadow / 5.0f) * kShadowStrength;
        float white = cover[y * plate.width + x];
        int offset = plate.Offset(x, y);
        byte[] shadowColor = { 72, 8, 0 };
        for (int channel = 0; channel < 3; ++channel) {
          float value = plate.bgra[offset + channel] * (1.0f - shadow) + shadowColor[channel] * shadow;
          plate.bgra[offset + channel] = (byte)(value * (1.0f - white) + 255.0f * white + 0.5f);
        }
      }
    }
  }

  [DllImport("kernel32.dll")]
  private static extern bool VirtualProtect(IntPtr address, IntPtr size, uint protection, out uint previous);

  // Compiled code of a method: its entry point may be a jump stub.
  private static IntPtr CodeOf(MethodInfo method) {
    RuntimeHelpers.PrepareMethod(method.MethodHandle);
    IntPtr code = method.MethodHandle.GetFunctionPointer();
    while (Marshal.ReadByte(code) == 0xE9) code = code + 5 + Marshal.ReadInt32(code + 1);
    return code;
  }

  // Overwrites a method's start with a jump to another of the same signature. The launcher's reader has an exception handler, so the
  // JIT never inlines it into a caller and every read passes here.
  private static void Redirect(MethodInfo source, MethodInfo target) {
    const uint kReadWriteExecute = 0x40;
    IntPtr from = CodeOf(source);
    IntPtr to = CodeOf(target);
    uint previous;
    if (!VirtualProtect(from, (IntPtr)5, kReadWriteExecute, out previous)) return;
    Marshal.WriteByte(from, 0xE9);
    Marshal.WriteInt32(from + 1, to.ToInt32() - from.ToInt32() - 5);
    VirtualProtect(from, (IntPtr)5, previous, out previous);
  }
}

}  // namespace OpenCross

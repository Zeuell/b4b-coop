// b4bmod: no-editor texture and material-instance mods for Back 4 Blood (issues #18/#21, epic #23).
// Reads cooked packages extracted from the game (`b4bmod extract`, i.e. pakx), writes edited cooked packages into a mod
// folder laid out like the game (<out>/Gobi/Content/...), ready for the add-on packer (`b4bmod pack`).
// Normally run through modkit/b4bmod.py (builds this on first use). modkit/docs/textures.md.
using System.Globalization;
using B4BMod;
using UAssetAPI;
using UAssetAPI.ExportTypes;
using UAssetAPI.PropertyTypes.Objects;
using UAssetAPI.PropertyTypes.Structs;
using UAssetAPI.UnrealTypes;

const string Usage = @"b4bmod <command> ...   (assets: /Game/... paths looked up under --src, or .uasset files)
  info <asset>...                         what it is: texture format/size/mips, MI parameters, referenced assets
  tree <asset>                            mesh -> material slots -> MIs -> textures (as far as extracted)
  export <asset> <out.png> [--mip N]      texture pixels to PNG (BC5 normal maps: Z rebuilt into blue)
  texture <asset> <in.png> -o <outdir> [--resize] [--quality fast|balanced|best]
                                          new pixels, same pixel format; the PNG's size is used as is unless
                                          --resize (scale to the original size); full mip chain rebuilt
  mi <asset> [list]                       material instance parameters
  mi <asset> set <param> <value> [set <param> <value>...] [parent <path>] -o <outdir>
                                          value: number (scalar), r,g,b[,a] (vector, linear 0-1),
                                          /Game/... texture path, or 'none'
  rename <asset> </Game/new/Path/Name> -o <outdir> [--ref </Game/old>=</Game/new>]... [--import-class </Game/pkg>=<Class>]...
                                          a copy of the asset as a new package (adds, replaces nothing); --ref points
                                          its references to other renamed copies (e.g. an MI's textures);
                                          --import-class sets the class of what it imports from that (new) package
                                          (community packages sometimes import an MI as a Material)
  dt <asset> [<row>] [--names]            data table rows and their fields (--names: row names only); a composite
                                          table lists the tables it is made of
Checks: texcheck <dir> (every texture re-writes byte-identically?), deps <asset>, props <asset>
Common: --src <dir containing Gobi/> (default $B4B_EXTRACT, else %LOCALAPPDATA%\b4b-coop\extract on Windows,
~/.local/share/b4b-coop/extract elsewhere).
Edits read an asset from <outdir> if it is already there, so several edits add up.";

if (args.Length == 0 || args[0] is "-h" or "--help" or "help") { Console.WriteLine(Usage); return 0; }
var opt = new Opts(args);
try
{
    switch (opt.Pos(0))
    {
        case "info": foreach (var a in opt.PosFrom(1)) Commands.Info(opt, a); break;
        case "tree": Commands.Tree(opt, opt.Pos(1)); break;
        case "export": Commands.Export(opt, opt.Pos(1), opt.Pos(2)); break;
        case "texture": Commands.TextureImport(opt, opt.Pos(1), opt.Pos(2)); break;
        case "mi": Commands.Mi(opt); break;
        case "texcheck": Commands.TexCheck(opt.Pos(1)); break;
        case "rename": Commands.Rename(opt, opt.Pos(1), opt.Pos(2)); break;
        case "deps": Commands.Deps(opt, opt.Pos(1)); break;
        case "props": Commands.Props(opt, opt.Pos(1)); break;
        case "dt": Commands.Dt(opt, opt.Pos(1), opt.Positional.Count > 2 ? opt.Pos(2) : null); break;
        default: Console.Error.WriteLine(Usage); return 2;
    }
}
catch (UsageException e) { Console.Error.WriteLine($"b4bmod: {e.Message}\n\n{Usage}"); return 2; }
catch (Exception e) { Console.Error.WriteLine($"b4bmod: {e.Message}"); return 1; }
return 0;

class UsageException(string m) : Exception(m);

class Opts
{
    public readonly List<string> Positional = new();
    public readonly Dictionary<string, string> Named = new();
    public readonly HashSet<string> Flags = new();
    public readonly List<string> Refs = new();
    public readonly List<string> ImportClasses = new();
    public Opts(string[] args)
    {
        for (int i = 0; i < args.Length; i++)
        {
            string s = args[i];
            if (s is "-o" or "--out" or "--src" or "--mip" or "--quality") { if (i + 1 >= args.Length) throw new UsageException($"{s} needs a value"); Named[s == "-o" ? "--out" : s] = args[++i]; }
            else if (s is "--ref") { if (i + 1 >= args.Length) throw new UsageException("--ref needs old=new"); Refs.Add(args[++i]); }
            else if (s is "--import-class") { if (i + 1 >= args.Length) throw new UsageException("--import-class needs /Game/pkg=Class"); ImportClasses.Add(args[++i]); }
            else if (s is "--resize" or "--names") Flags.Add(s);
            else Positional.Add(s);
        }
    }
    public string Pos(int i) => i < Positional.Count ? Positional[i] : throw new UsageException("missing argument");
    public IEnumerable<string> PosFrom(int i) => Positional.Skip(i);
    public string Src => Named.GetValueOrDefault("--src") ?? Environment.GetEnvironmentVariable("B4B_EXTRACT") ??
        (OperatingSystem.IsWindows()
            ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "b4b-coop", "extract")
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local/share/b4b-coop/extract"));
    public string Out => Named.GetValueOrDefault("--out") ?? throw new UsageException("-o <outdir> is required");
}

static class Commands
{
    public static UAsset Load(string p) => new UAsset(p, EngineVersion.VER_UE4_25);

    // /Game/X/Y[.Y] or a file path -> (file on disk, path relative to the game root: Gobi/Content/X/Y.uasset)
    public static (string file, string rel) Resolve(Opts o, string asset, string outdir = null)
    {
        string rel;
        if (asset.StartsWith("/Game/"))
        {
            var p = asset[6..];
            int dot = p.LastIndexOf('.'); if (dot > p.LastIndexOf('/')) p = p[..dot];
            rel = "Gobi/Content/" + p + ".uasset";
        }
        else
        {
            var full = Path.GetFullPath(asset.EndsWith(".uasset") ? asset : Path.ChangeExtension(asset, ".uasset"));
            int k = full.Replace('\\', '/').LastIndexOf("/Gobi/Content/", StringComparison.OrdinalIgnoreCase);
            if (k < 0) throw new Exception($"{asset}: not under a Gobi/Content/ folder, can't tell its game path");
            if (!File.Exists(full)) throw new Exception($"{full}: not found");
            rel = full.Replace('\\', '/')[(k + 1)..];
            if (outdir == null) return (full, rel);
            var inOut0 = Path.Combine(outdir, rel);
            return (File.Exists(inOut0) ? inOut0 : full, rel);
        }
        if (outdir != null)
        {
            var inOut = Path.Combine(outdir, rel);
            if (File.Exists(inOut)) { Console.WriteLine($"editing {inOut} (already in the mod folder)"); return (inOut, rel); }
        }
        var f = Path.Combine(o.Src, rel);
        if (!File.Exists(f)) throw new Exception($"{asset}: {f} not found (extract it first: b4bmod extract <asset or folder/*>, or --src)");
        return (f, rel);
    }

    public static string GamePath(string rel) => "/Game/" + rel["Gobi/Content/".Length..^".uasset".Length];

    static NormalExport MainExport(UAsset a) => a.Exports.OfType<NormalExport>().FirstOrDefault(e => e.bIsAsset)
        ?? a.Exports.OfType<NormalExport>().First();

    public static string ClassOf(UAsset a, Export e) => e.GetExportClassType().ToString();

    public static string ImportPath(UAsset a, FPackageIndex i)
    {
        if (i == null || i.Index == 0) return "None";
        if (i.IsExport()) return i.ToExport(a).ObjectName.ToString();
        var imp = i.ToImport(a);
        var outer = imp.OuterIndex;
        return outer.Index == 0 ? imp.ObjectName.ToString() : $"{ImportPath(a, outer)}.{imp.ObjectName}";
    }

    public static void Info(Opts o, string asset)
    {
        var (file, rel) = Resolve(o, asset);
        var a = Load(file);
        var e = MainExport(a);
        string cls = ClassOf(a, e);
        Console.WriteLine($"{GamePath(rel)}  [{cls}]  ({file})");
        if (cls == "Texture2D")
        {
            var t = Texture2DData.Read(a, e, file);
            var p = t.Platforms[0];
            Console.WriteLine($"  {p.PixelFormat} {p.SizeX}x{p.SizeY}, {p.Mips.Count} mips ({p.Mips.Count(m => m.InUbulk)} in .ubulk, " +
                $"{p.Mips.Count(m => m.Inline)} inline), sRGB {TexSrgb(e)}, {Prop(e, "CompressionSettings") ?? "TC_Default"}, " +
                $"LODGroup {Prop(e, "LODGroup") ?? "TEXTUREGROUP_World"}");
        }
        else if (cls is "MaterialInstanceConstant")
            Mi(o, a, e);
        else
        {
            foreach (var imp in a.Imports.Where(i => i.ClassName.ToString() is "Texture2D" or "MaterialInstanceConstant" or "Material"
                         or "SkeletalMesh" or "StaticMesh" or "PhysicsAsset" or "Skeleton" && PackageOf(a, i).StartsWith("/Game/")))
                Console.WriteLine($"  uses {imp.ClassName}: {PackageOf(a, imp)}");
            foreach (var ex in a.Exports.Where(x => x != e))
                Console.WriteLine($"  also exports {ClassOf(a, ex)} {ex.ObjectName}");
        }
    }

    static string PackageOf(UAsset a, Import imp)
    {
        var i = imp; while (i.OuterIndex.Index != 0) i = i.OuterIndex.ToImport(a);
        return i.ObjectName.ToString();
    }

    static string Prop(NormalExport e, string name)
    {
        var p = e.Data.FirstOrDefault(d => d.Name.ToString() == name);
        return p?.RawValue?.ToString();
    }

    static bool TexSrgb(NormalExport e) => Prop(e, "SRGB") is not ("False" or "false");

    // ---- tree: mesh -> MIs -> textures
    public static void Tree(Opts o, string asset, int depth = 0, HashSet<string> seen = null, string label = "")
    {
        seen ??= new();
        string pad = new(' ', depth * 2);
        (string file, string rel) r;
        try { r = Resolve(o, asset); }
        catch { Console.WriteLine($"{pad}{label}{asset}  (not extracted)"); return; }
        if (!seen.Add(r.rel)) { Console.WriteLine($"{pad}{label}{GamePath(r.rel)}  (see above)"); return; }
        var a = Load(r.file);
        var e = MainExport(a);
        string cls = ClassOf(a, e);
        string extra = "";
        if (cls == "Texture2D")
        {
            var p = Texture2DData.Read(a, e, r.file).Platforms[0];
            extra = $"  {p.PixelFormat} {p.SizeX}x{p.SizeY}";
        }
        Console.WriteLine($"{pad}{label}{GamePath(r.rel)}  [{cls}]{extra}");
        if (cls == "MaterialInstanceConstant")
        {
            var parent = e.Data.FirstOrDefault(d => d.Name.ToString() == "Parent") as ObjectPropertyData;
            if (e["TextureParameterValues"] is ArrayPropertyData arr)
                foreach (StructPropertyData s in arr.Value)
                {
                    var tex = ((ObjectPropertyData)s["ParameterValue"]).Value;
                    string texPkg = tex.Index == 0 ? "None" : PackageOf(a, tex.ToImport(a));
                    if (texPkg == "None") Console.WriteLine($"{pad}  '{ParamName(s)}' = None");
                    else Tree(o, texPkg, depth + 1, new HashSet<string>(), $"'{ParamName(s)}' = ");
                }
            if (parent != null && parent.Value.Index != 0)
            {
                var pimp = parent.Value.ToImport(a);
                if (pimp.ClassName.ToString() == "MaterialInstanceConstant") Tree(o, PackageOf(a, pimp), depth + 1, seen, "parent: ");
                else Console.WriteLine($"{pad}  parent: {PackageOf(a, pimp)}  [{pimp.ClassName}]");
            }
            return;
        }
        if (cls is "SkeletalMesh" or "StaticMesh" or "Material" || cls.EndsWith("_C") || cls == "BlueprintGeneratedClass")
            foreach (var imp in a.Imports.Where(i => i.ClassName.ToString() is "MaterialInstanceConstant" or "Texture2D" or "SkeletalMesh" or "StaticMesh"))
                if (PackageOf(a, imp) is var pk && pk.StartsWith("/Game/")) Tree(o, pk, depth + 1, seen);
    }

    public static string ParamName(StructPropertyData s) =>
        ((NamePropertyData)((StructPropertyData)s["ParameterInfo"])["Name"]).Value.ToString();

    // ---- texture export
    public static void Export(Opts o, string asset, string png)
    {
        var (file, _) = Resolve(o, asset);
        var a = Load(file);
        var e = MainExport(a);
        if (ClassOf(a, e) != "Texture2D") throw new Exception($"{asset} is a {ClassOf(a, e)}, not a Texture2D");
        var p = Texture2DData.Read(a, e, file).Platforms[0];
        int mip = int.Parse(o.Named.GetValueOrDefault("--mip") ?? "0");
        var m = p.Mips[mip];
        var rgba = Codec.Decode(p.PixelFormat, m.Data, m.SizeX, m.SizeY);
        Codec.WritePng(png, rgba, m.SizeX, m.SizeY);
        Console.WriteLine($"{png}: {m.SizeX}x{m.SizeY} from {p.PixelFormat} mip {mip}");
    }

    // ---- texture import
    public static void TextureImport(Opts o, string asset, string png)
    {
        string outdir = o.Out;
        var (file, rel) = Resolve(o, asset, outdir);
        var a = Load(file);
        var e = MainExport(a);
        if (ClassOf(a, e) != "Texture2D") throw new Exception($"{asset} is a {ClassOf(a, e)}, not a Texture2D");
        var t = Texture2DData.Read(a, e, file);
        if (t.Platforms.Count != 1) throw new Exception($"{t.Platforms.Count} cooked platform formats: not supported");
        var p = t.Platforms[0];
        bool srgb = TexSrgb(e);
        var (img, w, h) = Codec.ReadPng(png);
        if (o.Flags.Contains("--resize") && (w != p.SizeX || h != p.SizeY))
        {
            Console.WriteLine($"resizing {w}x{h} -> {p.SizeX}x{p.SizeY}");
            img = Codec.Resize(img, w, h, p.SizeX, p.SizeY, srgb); w = p.SizeX; h = p.SizeY;
        }
        else if (w != p.SizeX || h != p.SizeY)
            Console.WriteLine($"note: PNG is {w}x{h}, the original {p.SizeX}x{p.SizeY}; keeping {w}x{h} (--resize to match)");
        bool fullChain = p.Mips.Count > 1;
        var (bw, _) = Texture2DData.FormatInfo(p.PixelFormat);
        if (fullChain && ((w & (w - 1)) != 0 || (h & (h - 1)) != 0))
            throw new Exception($"{w}x{h}: a texture with mips needs power-of-two sides (or use --resize)");
        if (w % bw != 0 || h % bw != 0) throw new Exception($"{w}x{h}: sides must be multiples of {bw}");

        // Which mips stay inline: the original keeps every mip up to this size in the export (7 mips: 64..1), the
        // rest in the .ubulk (streamed). Same rule for the new size; templates for flags from the original.
        int inlineMax = p.Mips.Where(m => m.Inline).Select(m => Math.Max(m.SizeX, m.SizeY)).DefaultIfEmpty(0).Max();
        var inlineTpl = p.Mips.FirstOrDefault(m => m.Inline);
        var ubulkTpl = p.Mips.FirstOrDefault(m => m.InUbulk);
        int levels = fullChain ? (int)Math.Log2(Math.Max(w, h)) + 1 : 1;
        string quality = o.Named.GetValueOrDefault("--quality") ?? "balanced";
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var chain = Codec.MipChain(img, w, h, levels, srgb, p.PixelFormat == "PF_BC5");
        var mips = new List<Mip>();
        for (int i = 0; i < levels; i++)
        {
            var (px, mw, mh) = chain[i];
            bool inl = Math.Max(mw, mh) <= inlineMax || ubulkTpl == null;
            var tpl = inl ? inlineTpl : ubulkTpl;
            if (tpl == null) throw new Exception("no template mip for this storage");
            var data = Codec.Encode(p.PixelFormat, px, mw, mh, quality);
            if (data.Length != Texture2DData.MipBytes(p.PixelFormat, mw, mh)) throw new Exception($"mip {i}: encoder gave {data.Length} bytes");
            mips.Add(new Mip { Cooked = tpl.Cooked, Flags = tpl.Flags, Data = data, SizeX = mw, SizeY = mh, SizeZ = 1 });
        }
        Console.WriteLine($"encoded {levels} mips {p.PixelFormat} ({quality}, sRGB {srgb}) in {sw.Elapsed.TotalSeconds:F1} s");
        p.Mips = mips; p.SizeX = w; p.SizeY = h; p.FirstMip = 0;
        WriteTexture(a, e, t, outdir, rel);
    }

    static void WriteTexture(UAsset a, NormalExport e, Texture2DData t, string outdir, string rel)
    {
        // The native data holds absolute offsets (SkipOffset, inline mip offsets), which depend on where the export
        // lands: write once to learn it, then again with the right base (same length, so the base stays).
        // Mips without NoOffsetFixUp (community-cooked) are stored relative to BulkDataStartOffset, which moves too.
        long guess = Texture2DData.ExtrasBase(a, e), bulk = Texture2DData.BulkStart(a);
        byte[] ub = null;
        for (int pass = 0; ; pass++)
        {
            (e.Extras, ub) = t.Write(guess, bulk);
            a.WriteData();                      // updates the export map (SerialOffset/SerialSize) and the summary
            long real = Texture2DData.ExtrasBase(a, e), realBulk = Texture2DData.BulkStart(a);
            if (real == guess && realBulk == bulk) break;
            if (pass == 3) throw new Exception("export offset does not settle");
            guess = real; bulk = realBulk;
        }
        var outAsset = Path.Combine(outdir, rel);
        Directory.CreateDirectory(Path.GetDirectoryName(outAsset)!);
        a.Write(outAsset);
        var ubPath = Path.ChangeExtension(outAsset, ".ubulk");
        if (ub != null) File.WriteAllBytes(ubPath, ub); else if (File.Exists(ubPath)) File.Delete(ubPath);
        Console.WriteLine($"wrote {outAsset} (+ .uexp{(ub != null ? $", .ubulk {ub.Length} bytes" : "")})");
        Verify(outAsset);
    }

    // ---- new packages: a renamed copy of a game asset (the base of add-ons that add instead of replace)
    static (string pkg, string name) Split(string gamePath)
    {
        var p = gamePath; int dot = p.LastIndexOf('.'); if (dot > p.LastIndexOf('/')) p = p[..dot];
        if (!p.StartsWith("/Game/") || p.EndsWith("/")) throw new UsageException($"{gamePath}: expected /Game/Folder/Name");
        return (p, p[(p.LastIndexOf('/') + 1)..]);
    }

    public static void Rename(Opts o, string asset, string newPath)
    {
        string outdir = o.Out;
        var (file, rel) = Resolve(o, asset);
        var a = Load(file);
        var (oldPkg, oldName) = Split(GamePath(rel));
        var (newPkg, newName) = Split(newPath);
        // name map entries: the package path and the asset's name, plus each --ref (an import's package and name).
        // Entries are changed in place, so every FName index in the export data stays valid.
        var map = new Dictionary<string, string> { [oldPkg] = newPkg, [oldName] = newName };
        foreach (var r in o.Refs)
        {
            int eq = r.IndexOf('=');
            if (eq < 0) throw new UsageException($"--ref {r}: expected /Game/old=/Game/new");
            var (op, on) = Split(r[..eq]); var (np, nn) = Split(r[(eq + 1)..]);
            map[op] = np; map[on] = nn;
        }
        var names = a.GetNameMapIndexList();
        int changed = 0;
        for (int i = 0; i < names.Count; i++)
            if (names[i]?.Value != null && map.TryGetValue(names[i].Value, out var nv)) { a.SetNameReference(i, new FString(nv)); changed++; }
        // imports whose class doesn't match the object they name (the loader refuses them: "class mismatch")
        foreach (var ic in o.ImportClasses)
        {
            int eq = ic.IndexOf('=');
            if (eq < 0) throw new UsageException($"--import-class {ic}: expected /Game/pkg=Class");
            string pkg = ic[..eq], cls = ic[(eq + 1)..];
            foreach (var imp in a.Imports)
            {
                if (imp.OuterIndex.Index >= 0 || imp.ClassName.Value.Value == "Package") continue;
                var outer = a.Imports[-imp.OuterIndex.Index - 1];
                if (outer.ClassName.Value.Value != "Package" || outer.ObjectName.Value.Value != pkg || imp.ClassName.Value.Value == cls) continue;
                Console.WriteLine($"  import {pkg}.{imp.ObjectName.Value.Value}: class {imp.ClassName.Value.Value} -> {cls}");
                a.AddNameReference(new FString(cls));
                imp.ClassName = new FName(a, cls);
                changed++;
            }
        }
        if (a.AssetRegistryRecords != null)
            foreach (var rec in a.AssetRegistryRecords)
            {
                if (map.TryGetValue(rec.Path, out var np2)) rec.Path = np2;
                foreach (var k in rec.TagMap.Keys.ToList())
                    foreach (var (ov, nv2) in map)
                        if (ov.StartsWith("/Game/") && rec.TagMap[k].Contains(ov + "."))
                            rec.TagMap[k] = rec.TagMap[k].Replace(ov + "." + ov[(ov.LastIndexOf('/') + 1)..], nv2 + "." + nv2[(nv2.LastIndexOf('/') + 1)..]);
            }
        var newRel = "Gobi/Content/" + newPkg["/Game/".Length..] + ".uasset";
        var e = MainExport(a);
        Console.WriteLine($"{oldPkg} -> {newPkg}: {changed} name(s) changed ({ClassOf(a, e)})");
        if (ClassOf(a, e) == "Texture2D")
        {
            // textures hold absolute offsets that move with the (now longer or shorter) header: rewrite them
            var t = Texture2DData.Read(a, e, file);
            WriteTexture(a, e, t, outdir, newRel);
            return;
        }
        var outAsset = Path.Combine(outdir, newRel);
        Directory.CreateDirectory(Path.GetDirectoryName(outAsset)!);
        a.Write(outAsset);
        foreach (var ext in new[] { ".ubulk", ".uptnl" })
            if (File.Exists(Path.ChangeExtension(file, ext))) File.Copy(Path.ChangeExtension(file, ext), Path.ChangeExtension(outAsset, ext), true);
        Console.WriteLine($"wrote {outAsset} (+ .uexp)");
    }

    static void Verify(string outAsset)
    {
        var a = Load(outAsset);
        var e = MainExport(a);
        var t = Texture2DData.Read(a, e, outAsset);
        var p = t.Platforms[0];
        Console.WriteLine($"check: re-read {p.PixelFormat} {p.SizeX}x{p.SizeY}, {p.Mips.Count} mips, offsets consistent");
    }

    // ---- material instances
    public static void Mi(Opts o)
    {
        string asset = o.Pos(1);
        var rest = o.PosFrom(2).ToList();
        if (rest.Count == 0 || rest[0] == "list")
        {
            var (f0, _) = Resolve(o, asset);
            var a0 = Load(f0);
            Mi(o, a0, MainExport(a0));
            return;
        }
        string outdir = o.Out;
        var (file, rel) = Resolve(o, asset, outdir);
        var a = Load(file);
        var e = MainExport(a);
        if (ClassOf(a, e) != "MaterialInstanceConstant") throw new Exception($"{asset} is a {ClassOf(a, e)}, not a MaterialInstanceConstant");
        for (int i = 0; i < rest.Count;)
        {
            if (rest[i] == "set" && i + 2 < rest.Count) { MiSet(a, e, rest[i + 1], rest[i + 2]); i += 3; }
            else if (rest[i] == "parent" && i + 1 < rest.Count) { MiParent(a, e, rest[i + 1]); i += 2; }
            else throw new UsageException($"mi: expected 'set <param> <value>' or 'parent <path>' at '{rest[i]}'");
        }
        var outAsset = Path.Combine(outdir, rel);
        Directory.CreateDirectory(Path.GetDirectoryName(outAsset)!);
        a.Write(outAsset);
        Console.WriteLine($"wrote {outAsset} (+ .uexp)");
        var chk = Load(outAsset);
        Mi(o, chk, MainExport(chk));
    }

    static void Mi(Opts o, UAsset a, NormalExport e)
    {
        var parent = e.Data.FirstOrDefault(d => d.Name.ToString() == "Parent") as ObjectPropertyData;
        Console.WriteLine($"  parent: {(parent == null ? "None" : ImportPath(a, parent.Value))}");
        foreach (var (arrName, kind) in new[] { ("ScalarParameterValues", "scalar"), ("VectorParameterValues", "vector"), ("TextureParameterValues", "texture") })
        {
            if (e[arrName] is not ArrayPropertyData arr) continue;
            foreach (StructPropertyData s in arr.Value)
            {
                string v = s["ParameterValue"] switch
                {
                    FloatPropertyData f => f.Value.ToString(CultureInfo.InvariantCulture),
                    LinearColorPropertyData c => string.Join(",", new[] { c.Value.R, c.Value.G, c.Value.B, c.Value.A }.Select(x => x.ToString("0.###", CultureInfo.InvariantCulture))),
                    StructPropertyData sc when sc.Value.Count == 1 && sc.Value[0] is LinearColorPropertyData c2 =>
                        string.Join(",", new[] { c2.Value.R, c2.Value.G, c2.Value.B, c2.Value.A }.Select(x => x.ToString("0.###", CultureInfo.InvariantCulture))),
                    ObjectPropertyData ob => ob.Value.Index == 0 ? "None" : PackageOf(a, ob.Value.ToImport(a)),
                    var x => x?.RawValue?.ToString() ?? "?",
                };
                Console.WriteLine($"  {kind,-7} '{ParamName(s)}' = {v}");
            }
        }
        if (e["StaticParameters"] is StructPropertyData sp && sp.Value.OfType<ArrayPropertyData>().Sum(x => x.Value.Length) is > 0 and var ns)
            Console.WriteLine($"  ({ns} static switch/mask parameters: fixed, changing them needs shaders)");
    }

    static StructPropertyData FindParam(NormalExport e, string arrName, string param, out ArrayPropertyData arr)
    {
        arr = e[arrName] as ArrayPropertyData;
        if (arr == null) return null;
        foreach (StructPropertyData s in arr.Value)
            if (string.Equals(ParamName(s), param, StringComparison.OrdinalIgnoreCase)) return s;
        return null;
    }

    static void MiSet(UAsset a, NormalExport e, string param, string value)
    {
        string kind = value.StartsWith("/") || value.Equals("none", StringComparison.OrdinalIgnoreCase) ? "texture"
            : value.Contains(',') ? "vector" : "scalar";
        string arrName = kind switch { "texture" => "TextureParameterValues", "vector" => "VectorParameterValues", _ => "ScalarParameterValues" };
        var s = FindParam(e, arrName, param, out var arr);
        if (s == null)
        {
            // Not overridden yet: clone an existing entry of the same kind and rename it. The parent material must
            // have this parameter (the engine ignores unknown names); names are case-sensitive in the engine.
            if (arr == null || arr.Value.Length == 0)
                throw new Exception($"'{param}': this MI overrides no {kind} parameter yet, so there is no entry to copy " +
                                    $"(not supported; pick an MI that already overrides a {kind} parameter)");
            s = (StructPropertyData)arr.Value[0].Clone();
            a.AddNameReference(new FString(param));
            var info = (StructPropertyData)s["ParameterInfo"];
            ((NamePropertyData)info["Name"]).Value = new FName(a, param);
            if (s["ExpressionGUID"] is GuidPropertyData gp) gp.Value = Guid.Empty;
            else if (s["ExpressionGUID"] is StructPropertyData g && g.Value.Count == 1 && g.Value[0] is GuidPropertyData gp2) gp2.Value = Guid.Empty;
            arr.Value = arr.Value.Append(s).ToArray();
            Console.WriteLine($"adding {kind} '{param}' (not overridden before; the parent material must have it, " +
                              "names are case-sensitive in game; the old value below is the copied entry's)");
        }
        switch (s["ParameterValue"])
        {
            case FloatPropertyData f:
                var nv = float.Parse(value, CultureInfo.InvariantCulture);
                Console.WriteLine($"{param}: {f.Value.ToString(CultureInfo.InvariantCulture)} -> {nv.ToString(CultureInfo.InvariantCulture)}");
                f.Value = nv; break;
            case StructPropertyData sc when sc.Value.Count == 1 && sc.Value[0] is LinearColorPropertyData c:
                SetColor(c, param, value); break;
            case LinearColorPropertyData c:
                SetColor(c, param, value); break;
            case ObjectPropertyData ob:
                string old = ob.Value.Index == 0 ? "None" : PackageOf(a, ob.Value.ToImport(a));
                ob.Value = value.Equals("none", StringComparison.OrdinalIgnoreCase) ? FPackageIndex.FromRawIndex(0) : TextureImport(a, e, value);
                Console.WriteLine($"{param}: {old} -> {value}");
                break;
            default: throw new Exception($"'{param}': unexpected value type {s["ParameterValue"]?.GetType().Name}");
        }
    }

    static void SetColor(LinearColorPropertyData c, string param, string value)
    {
        var v = value.Split(',').Select(x => float.Parse(x, CultureInfo.InvariantCulture)).ToArray();
        if (v.Length is < 3 or > 4) throw new Exception($"{param}: vector value is r,g,b[,a]");
        var old = c.Value;
        c.Value = new FLinearColor(v[0], v[1], v[2], v.Length == 4 ? v[3] : old.A);
        Console.WriteLine($"{param}: {old.R},{old.G},{old.B},{old.A} -> {c.Value.R},{c.Value.G},{c.Value.B},{c.Value.A}");
    }

    // Import for a texture package path (/Game/.../Name[.Name]), reusing existing imports; also listed as a
    // create-before-serialization dependency of the MI, like the cooker does for the MI's own textures.
    static FPackageIndex TextureImport(UAsset a, NormalExport e, string path) => ObjectImport(a, e, path, "/Script/Engine", "Texture2D");

    static FPackageIndex ObjectImport(UAsset a, NormalExport e, string path, string classPkg, string cls)
    {
        var pkgPath = path; var dot = pkgPath.LastIndexOf('.');
        if (dot > pkgPath.LastIndexOf('/')) pkgPath = pkgPath[..dot];
        var objName = pkgPath[(pkgPath.LastIndexOf('/') + 1)..];
        FPackageIndex pkgImp = null, objImp = null;
        for (int i = 0; i < a.Imports.Count; i++)
        {
            var imp = a.Imports[i];
            if (imp.OuterIndex.Index == 0 && imp.ObjectName.ToString() == pkgPath) pkgImp = FPackageIndex.FromImport(i);
        }
        if (pkgImp != null)
            for (int i = 0; i < a.Imports.Count; i++)
                if (a.Imports[i].OuterIndex.Index == pkgImp.Index && a.Imports[i].ObjectName.ToString() == objName) objImp = FPackageIndex.FromImport(i);
        if (objImp != null) return objImp;
        foreach (var n in new[] { pkgPath, objName, classPkg, cls, "/Script/CoreUObject", "Package" }) a.AddNameReference(new FString(n));
        pkgImp ??= a.AddImport(new Import("/Script/CoreUObject", "Package", FPackageIndex.FromRawIndex(0), pkgPath, false, a));
        objImp = a.AddImport(new Import(classPkg, cls, pkgImp, objName, false, a));
        // the cooker lists every import an export references among its preload dependencies (EDL): the MI's own
        // textures are in CreateBeforeSerialization
        if (!e.CreateBeforeSerializationDependencies.Any(d => d.Index == objImp.Index))
            e.CreateBeforeSerializationDependencies.Add(objImp);
        return objImp;
    }

    static void MiParent(UAsset a, NormalExport e, string path)
    {
        var parent = e.Data.FirstOrDefault(d => d.Name.ToString() == "Parent") as ObjectPropertyData
            ?? throw new Exception("MI has no Parent property");
        string cls = path.Contains("/Masters/") && !path.EndsWith("_MI") ? "Material" : "MaterialInstanceConstant";
        string old = ImportPath(a, parent.Value);
        parent.Value = ObjectImport(a, e, path, "/Script/Engine", cls);
        Console.WriteLine($"parent: {old} -> {path} ({cls})");
    }

    // ---- dev: imports and the exports' preload dependencies (event-driven loader)
    public static void Deps(Opts o, string asset)
    {
        var (file, _) = Resolve(o, asset);
        var a = Load(file);
        for (int i = 0; i < a.Imports.Count; i++) Console.WriteLine($"import {-i - 1}: {a.Imports[i].ClassName} {ImportPath(a, FPackageIndex.FromImport(i))}");
        foreach (var e in a.Exports)
        {
            Console.WriteLine($"export {e.ObjectName}: SerialOffset {e.SerialOffset} SerialSize {e.SerialSize}");
            Console.WriteLine($"  SerializationBeforeSerialization {string.Join(" ", e.SerializationBeforeSerializationDependencies.Select(d => d.Index))}");
            Console.WriteLine($"  CreateBeforeSerialization {string.Join(" ", e.CreateBeforeSerializationDependencies.Select(d => d.Index))}");
            Console.WriteLine($"  SerializationBeforeCreate {string.Join(" ", e.SerializationBeforeCreateDependencies.Select(d => d.Index))}");
            Console.WriteLine($"  CreateBeforeCreate {string.Join(" ", e.CreateBeforeCreateDependencies.Select(d => d.Index))}");
        }
    }

    // ---- dev: top-level tagged properties of the main export
    public static void Props(Opts o, string asset)
    {
        var (file, _) = Resolve(o, asset);
        var a = Load(file);
        var e = MainExport(a);
        foreach (var p in e.Data)
        {
            string v = p switch
            {
                ArrayPropertyData ar => $"[{ar.Value.Length}] " + string.Join(", ", ar.Value.Take(40).Select(x => x is ObjectPropertyData ob ? ImportPath(a, ob.Value) : x.PropertyType.ToString())),
                ObjectPropertyData ob => ImportPath(a, ob.Value),
                _ => p.RawValue?.ToString() ?? p.PropertyType.ToString(),
            };
            Console.WriteLine($"{p.Name} ({p.PropertyType}): {v}");
        }
        Console.WriteLine($"native tail {e.Extras.Length} bytes");
    }

    // ---- data tables: rows and their fields (read only). A CompositeDataTable has no rows of its own: its
    // ParentTables are listed; dump those.
    static string PropText(UAsset a, PropertyData p, int depth = 0)
    {
        switch (p)
        {
            case ObjectPropertyData ob: return ImportPath(a, ob.Value);
            case SoftObjectPropertyData so: return so.Value.AssetPath.AssetName.ToString();
            case NamePropertyData nm: return nm.Value.ToString();
            case TextPropertyData tx: return tx.Value?.ToString() ?? (tx.CultureInvariantString?.ToString() ?? "");
            case StrPropertyData st: return st.Value?.ToString() ?? "";
            case EnumPropertyData en: return en.Value.ToString();
            case BytePropertyData bp: return bp.ByteType == BytePropertyType.FName ? bp.EnumValue.ToString() : bp.Value.ToString();
            case BoolPropertyData bo: return bo.Value ? "true" : "false";
            case ArrayPropertyData ar: return "[" + string.Join(", ", ar.Value.Take(depth > 1 ? 4 : 40).Select(x => PropText(a, x, depth + 1))) + (ar.Value.Length > 40 ? ", ..." : "") + "]";
            case StructPropertyData sp when depth < 3: return "{" + string.Join(", ", sp.Value.Select(x => $"{x.Name}={PropText(a, x, depth + 1)}")) + "}";
            case StructPropertyData sp: return $"{{{sp.StructType} x{sp.Value.Count}}}";
            case MapPropertyData mp: return "{" + string.Join(", ", mp.Value.Take(20).Select(kv => $"{PropText(a, kv.Key, depth + 1)}: {PropText(a, kv.Value, depth + 1)}")) + "}";
            default: return p.RawValue?.ToString() ?? p.PropertyType.ToString();
        }
    }
    public static void Dt(Opts o, string asset, string row)
    {
        var (file, _) = Resolve(o, asset);
        var a = Load(file);
        var e = MainExport(a);
        if (e is not DataTableExport dt)
        {
            var parents = e.Data.OfType<ArrayPropertyData>().FirstOrDefault(p => p.Name.ToString() == "ParentTables");
            if (parents == null) throw new Exception($"{asset}: {ClassOf(a, e)} is not a data table");
            Console.WriteLine($"{asset}: {ClassOf(a, e)} of {parents.Value.Length} tables (dump each one):");
            foreach (var p in parents.Value.OfType<ObjectPropertyData>()) Console.WriteLine("  " + ImportPath(a, p.Value));
            return;
        }
        var rs = e.Data.OfType<ObjectPropertyData>().FirstOrDefault(p => p.Name.ToString() == "RowStruct");
        var rows = dt.Table.Data.Where(r => row == null || string.Equals(r.Name.ToString(), row, StringComparison.OrdinalIgnoreCase)).ToList();
        if (row != null && rows.Count == 0) throw new Exception($"{asset}: no row '{row}' ({dt.Table.Data.Count} rows; `dt {asset}` lists them)");
        if (o.Flags.Contains("--names"))
        {
            foreach (var r in rows) Console.WriteLine(r.Name);
            return;
        }
        Console.WriteLine($"{asset}: {dt.Table.Data.Count} rows of {(rs != null ? ImportPath(a, rs.Value) : "?")}{(row != null ? $", row {row}" : "")}");
        foreach (var r in rows)
        {
            Console.WriteLine($"[{r.Name}]");
            foreach (var p in r.Value) Console.WriteLine($"  {p.Name} ({p.PropertyType}): {PropText(a, p)}");
        }
    }

    // ---- dev: every texture under a dir parses and re-writes byte-identically
    public static void TexCheck(string dir)
    {
        int ok = 0, bad = 0;
        foreach (var path in Directory.GetFiles(dir, "*.uasset", SearchOption.AllDirectories).OrderBy(s => s))
        {
            UAsset a;
            try { a = Load(path); } catch { continue; }
            var e = a.Exports.OfType<NormalExport>().FirstOrDefault(x => x.bIsAsset && ClassOf(a, x) == "Texture2D");
            if (e == null) continue;
            try
            {
                var t = Texture2DData.Read(a, e, path);
                var (ex, ub) = t.Write(Texture2DData.ExtrasBase(a, e), Texture2DData.BulkStart(a));
                bool same = ex.AsSpan().SequenceEqual(e.Extras) &&
                            (t.UbulkPath == null ? ub == null : ub != null && ub.AsSpan().SequenceEqual(File.ReadAllBytes(t.UbulkPath)));
                var p = t.Platforms[0];
                Console.WriteLine($"{(same ? "OK " : "DIFF")} {Path.GetFileNameWithoutExtension(path)} {p.PixelFormat} {p.SizeX}x{p.SizeY} " +
                    $"mips {p.Mips.Count} ubulk {p.Mips.Count(m => m.InUbulk)} inline {p.Mips.Count(m => m.Inline)}");
                if (same) ok++; else bad++;
            }
            catch (Exception ex) { Console.WriteLine($"FAIL {path}: {ex.Message}"); bad++; }
        }
        Console.WriteLine($"{ok} identical, {bad} not");
    }
}

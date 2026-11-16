using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using CUE4Parse.Compression;
using CUE4Parse.FileProvider;
using CUE4Parse.FileProvider.Objects;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.Animation;
using CUE4Parse.UE4.Assets.Exports.SkeletalMesh;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.UObject;
using CUE4Parse.UE4.Versions;
using Newtonsoft.Json;

// Offline investigation tool for Déraciné's cooked Unreal assets. Read-only: it mounts the pak,
// reads package headers and specific assets, and writes manifests/reports. It never extracts whole
// assets to disk and never writes into the game folder.
static class Program
{
    const string PakDir = @"D:\Temp\Deracine\1.0\Image0\gameproject\content\paks";
    const string OodleSrc = @"C:\Users\Arukas\Downloads\oo2core_9_win64.dll";
    const string OutDir = @"M:\Development\Deracine-work\_build\handrig";

    static readonly string[] FingerKeywords =
        { "finger", "thumb", "index", "middle", "ring", "pinky", "phalanx", "metacarp" };

    static DefaultFileProvider Provider = null!;
    static readonly Dictionary<string, string> SkeletonPath = new(StringComparer.OrdinalIgnoreCase);
    static readonly Dictionary<string, List<string>> BoneNamesCache = new(StringComparer.OrdinalIgnoreCase);

    static void Main(string[] args)
    {
        Directory.CreateDirectory(OutDir);
        Init();
        switch (args.Length > 0 ? args[0] : "inventory")
        {
            case "inventory": Inventory(); break;
            case "skeleton": DumpSkeleton(args[1]); break;
            case "mesh": DumpMesh(args[1]); break;
            case "anim": DumpAnim(args[1]); break;
            case "posefinger": PoseFinger(args[1]); break;
            case "anims": ScanAnims(args.Length > 1 ? args[1] : ""); break;
            case "names": Names(args.Length > 1 ? args[1] : ""); break;
            default: Console.WriteLine("modes: inventory|skeleton|mesh|anim|posefinger|anims|names"); break;
        }
    }

    static void Init()
    {
        var oodle = Path.Combine(AppContext.BaseDirectory, "oo2core_9_win64.dll");
        if (File.Exists(OodleSrc)) File.Copy(OodleSrc, oodle, true);
        if (File.Exists(oodle)) OodleHelper.Initialize(oodle);
        var game = Environment.GetEnvironmentVariable("UEVERSION") switch
        {
            "419" => EGame.GAME_UE4_19,
            "420" => EGame.GAME_UE4_20,
            _ => EGame.GAME_UE4_18,
        };
        Provider = new DefaultFileProvider(PakDir, SearchOption.TopDirectoryOnly,
            new VersionContainer(game), StringComparer.OrdinalIgnoreCase);
        Provider.Initialize();
        Provider.Mount();
        Console.WriteLine($"provider: {Provider.Files.Count} files, {game}");
    }

    static string FullPath(GameFile file) =>
        file.Path.EndsWith("." + file.Extension, StringComparison.OrdinalIgnoreCase)
            ? file.Path
            : file.Path + "." + file.Extension;

    static string AssetPath(string path) =>
        path.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase) ? path[..^7] : path;

    static bool IsGameAsset(GameFile file, string filter) =>
        file.Extension == "uasset" &&
        FullPath(file).StartsWith("GameProject/", StringComparison.OrdinalIgnoreCase) &&
        FullPath(file).Contains(filter, StringComparison.OrdinalIgnoreCase);

    static bool IsFinger(string name) =>
        FingerKeywords.Any(k => name.Contains(k, StringComparison.OrdinalIgnoreCase));

    static string Safe(string path) =>
        string.Concat(path.Replace("GameProject/Content/", "").Select(c => char.IsLetterOrDigit(c) ? c : '_'));

    static void WriteJson(string name, object document) =>
        File.WriteAllText(Path.Combine(OutDir, name), JsonConvert.SerializeObject(document, Formatting.Indented));

    static object? Member(object? target, string name)
    {
        if (target is null) return null;
        var type = target.GetType();
        var property = type.GetProperty(name);
        if (property != null) return property.GetValue(target);
        return type.GetField(name)?.GetValue(target);
    }

    static string ResolveName(IPackage package, FPackageIndex index)
    {
        if (index.IsNull) return "";
        try
        {
            object? name = package.ResolvePackageIndex(index)?.Name;
            return name?.ToString() ?? index.ToString() ?? "";
        }
        catch
        {
            return "";
        }
    }

    // -- inventory ---------------------------------------------------------------------------------

    static void Inventory()
    {
        var exports = new StringBuilder("package,export,class\n");
        var references = new StringBuilder("package,name\n");
        var classes = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        int packages = 0, failed = 0;

        foreach (var file in Provider.Files.Values.Where(f => IsGameAsset(f, "")).OrderBy(f => FullPath(f)))
        {
            var path = FullPath(file);
            packages++;
            try
            {
                var package = Provider.LoadPackage(path);
                foreach (var name in package.NameMap)
                {
                    if (!string.IsNullOrEmpty(name.Name) && LooksLikeAsset(name.Name))
                        references.Append(path).Append(',').Append(name.Name).Append('\n');
                }
                foreach (var export in package.GetExports())
                {
                    var klass = export.ExportType;
                    classes[klass] = classes.GetValueOrDefault(klass) + 1;
                    exports.Append(path).Append(',').Append(export.Name).Append(',').Append(klass).Append('\n');
                }
            }
            catch (Exception ex)
            {
                failed++;
                Console.Error.WriteLine($"{path}: {ex.GetType().Name}: {ex.Message}");
            }
        }

        File.WriteAllText(Path.Combine(OutDir, "inventory-exports.csv"), exports.ToString());
        File.WriteAllText(Path.Combine(OutDir, "inventory-references.csv"), references.ToString());
        Console.WriteLine($"packages={packages} failed={failed}");
        foreach (var (klass, count) in classes.OrderByDescending(kv => kv.Value).Take(24))
            Console.WriteLine($"{count,6}  {klass}");
    }

    static bool LooksLikeAsset(string name)
    {
        if (name.Length is < 3 or > 80 || name.Contains('/')) return false;
        var underscore = name.IndexOf('_');
        if (underscore <= 0) return false;
        for (var i = 0; i < underscore; i++)
            if (!char.IsLetterOrDigit(name[i])) return false;
        return char.IsUpper(name[0]);
    }

    static void Names(string filter)
    {
        var hits = Provider.Files.Values.Where(f => IsGameAsset(f, filter)).Select(FullPath).OrderBy(p => p).ToList();
        foreach (var hit in hits) Console.WriteLine(hit);
        Console.WriteLine($"count={hits.Count}");
    }

    // -- skeleton ----------------------------------------------------------------------------------

    static void BuildSkeletonIndex()
    {
        if (SkeletonPath.Count > 0) return;
        foreach (var file in Provider.Files.Values.Where(f => IsGameAsset(f, "")))
        {
            var path = FullPath(file);
            try
            {
                var package = Provider.LoadPackage(path);
                foreach (var export in package.GetExports())
                {
                    if (export.ExportType.Contains("Skeleton") && !export.ExportType.Contains("SkeletalMesh"))
                        SkeletonPath[export.Name] = path;
                }
            }
            catch
            {
                // Unreadable packages cannot name a skeleton; the inventory records them.
            }
        }
        Console.WriteLine($"skeleton index: {SkeletonPath.Count}");
    }

    static List<string> BoneNames(string skeletonName)
    {
        if (string.IsNullOrEmpty(skeletonName)) return new List<string>();
        if (BoneNamesCache.TryGetValue(skeletonName, out var cached)) return cached;
        BuildSkeletonIndex();
        var names = new List<string>();
        if (SkeletonPath.TryGetValue(skeletonName, out var path))
        {
            var skeleton = Provider.LoadPackageObject<USkeleton>(AssetPath(path));
            names = skeleton.ReferenceSkeleton.FinalRefBoneInfo.Select(b => b.Name.Text).ToList();
        }
        BoneNamesCache[skeletonName] = names;
        return names;
    }

    sealed class Bone
    {
        public int Index;
        public string Name = "";
        public int Parent;
        public double[] Translation = Array.Empty<double>();
        public double[] Rotation = Array.Empty<double>();
    }

    static void DumpSkeleton(string path)
    {
        var skeleton = Provider.LoadPackageObject<USkeleton>(AssetPath(path));
        var reference = skeleton.ReferenceSkeleton;
        var bones = new List<Bone>();
        for (var i = 0; i < reference.FinalRefBoneInfo.Length; i++)
        {
            var info = reference.FinalRefBoneInfo[i];
            var pose = reference.FinalRefBonePose.Length > i ? reference.FinalRefBonePose[i] : default;
            bones.Add(new Bone
            {
                Index = i,
                Name = info.Name.Text,
                Parent = info.ParentIndex,
                Translation = new[] { (double)pose.Translation.X, (double)pose.Translation.Y, (double)pose.Translation.Z },
                Rotation = new[] { (double)pose.Rotation.X, (double)pose.Rotation.Y, (double)pose.Rotation.Z, (double)pose.Rotation.W },
            });
        }

        var worldPos = new Vec[bones.Count];
        var worldRot = new double[bones.Count][];
        for (var i = 0; i < bones.Count; i++)
        {
            var bone = bones[i];
            if (bone.Parent < 0)
            {
                worldRot[i] = bone.Rotation;
                worldPos[i] = new Vec(bone.Translation[0], bone.Translation[1], bone.Translation[2]);
            }
            else
            {
                worldRot[i] = QuatMul(worldRot[bone.Parent], bone.Rotation);
                var local = QuatRotate(worldRot[bone.Parent], bone.Translation);
                worldPos[i] = worldPos[bone.Parent] + local;
            }
        }

        var chains = FingerChains(bones, worldPos);
        var sockets = SocketsOf(path);
        var virtualBones = skeleton.VirtualBones.Select(vb => new
        {
            name = vb.VirtualBoneName.Text, source = vb.SourceBoneName.Text, target = vb.TargetBoneName.Text,
        }).ToList();

        var document = new
        {
            asset = path,
            boneCount = bones.Count,
            fingerBones = bones.Count(b => IsFinger(b.Name)),
            helperBones = bones.Count(b => b.Name.Contains("Twist", StringComparison.OrdinalIgnoreCase) ||
                                            b.Name.EndsWith("Nub", StringComparison.Ordinal)),
            fingers = chains,
            virtualBones,
            sockets,
            bones = bones.Select((b, i) => new
            {
                b.Index, b.Name, b.Parent,
                finger = IsFinger(b.Name),
                translation = b.Translation,
                rotation = b.Rotation,
                worldPosition = new[] { worldPos[i].X, worldPos[i].Y, worldPos[i].Z },
            }).ToList(),
        };
        WriteJson("skeleton-" + Safe(path) + ".json", document);

        Console.WriteLine($"skeleton {path}: bones={bones.Count} fingers={document.fingerBones} " +
                          $"helpers={document.helperBones} sockets={sockets.Count} chains={chains.Count}");
        foreach (var chain in chains)
            Console.WriteLine($"  {chain.Side} digit {chain.Digit}: {string.Join(">", chain.Joints)} " +
                              $"len={chain.Length:F1} thumb={chain.ThumbScore:F2}");
        foreach (var socket in sockets) Console.WriteLine($"  socket {socket}");
    }

    record struct Vec(double X, double Y, double Z)
    {
        public static Vec operator +(Vec a, Vec b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
        public static Vec operator -(Vec a, Vec b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
        public double Length => Math.Sqrt(X * X + Y * Y + Z * Z);
        public Vec Normalized => Length <= 1e-9 ? new Vec(0, 0, 0) : new Vec(X / Length, Y / Length, Z / Length);
        public static double Dot(Vec a, Vec b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    }

    static double[] QuatMul(double[] a, double[] b) => new[]
    {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };

    static Vec QuatRotate(double[] q, double[] v)
    {
        var (x, y, z, w) = (q[0], q[1], q[2], q[3]);
        var ix = w * v[0] + y * v[2] - z * v[1];
        var iy = w * v[1] + z * v[0] - x * v[2];
        var iz = w * v[2] + x * v[1] - y * v[0];
        var iw = -x * v[0] - y * v[1] - z * v[2];
        return new Vec(ix * w + iw * -x + iy * -z - iz * -y,
                       iy * w + iw * -y + iz * -x - ix * -z,
                       iz * w + iw * -z + ix * -y - iy * -x);
    }

    sealed class FingerChain
    {
        public string Side = "";
        public int Digit;
        public List<string> Joints = new();
        public double Length;
        public double ThumbScore;
        public double[] Tip = Array.Empty<double>();
    }

    // Groups the FingerN/N1/N2 chains per hand and measures them, so which digit is the thumb is
    // answered from the reference pose geometry rather than assumed from the name.
    static List<FingerChain> FingerChains(List<Bone> bones, Vec[] world)
    {
        var chains = new List<FingerChain>();
        foreach (var side in new[] { "R-", "L-" })
        {
            var hand = bones.FirstOrDefault(b => b.Name == side + "Hand");
            var forearm = bones.FirstOrDefault(b => b.Name == side + "Forearm");
            if (hand == null) continue;
            var palmAxis = forearm != null
                ? (world[hand.Index] - world[forearm.Index]).Normalized
                : new Vec(1, 0, 0);
            for (var digit = 0; digit <= 4; digit++)
            {
                var root = $"{side}Finger{digit}";
                var joints = bones.Where(b => b.Name == root || b.Name == root + "1" || b.Name == root + "2")
                    .OrderBy(b => b.Name.Length).ThenBy(b => b.Name).ToList();
                if (joints.Count == 0) continue;
                var length = 0.0;
                for (var i = 1; i < joints.Count; i++) length += (world[joints[i].Index] - world[joints[i - 1].Index]).Length;
                var basePos = world[joints[0].Index];
                var tip = world[joints[^1].Index];
                var direction = (tip - basePos).Normalized;
                chains.Add(new FingerChain
                {
                    Side = side.TrimEnd('-'),
                    Digit = digit,
                    Joints = joints.Select(j => j.Name).ToList(),
                    Length = length,
                    ThumbScore = 1.0 - Math.Abs(Vec.Dot(direction, palmAxis)),
                    Tip = new[] { tip.X, tip.Y, tip.Z },
                });
            }
        }
        return chains;
    }

    static List<object> SocketsOf(string path)
    {
        var sockets = new List<object>();
        try
        {
            var package = Provider.LoadPackage(path);
            foreach (var export in package.GetExports().Where(e => e.ExportType.Contains("SkeletalMeshSocket")))
            {
                sockets.Add(new
                {
                    name = Member(export, "SocketName")?.ToString(),
                    bone = Member(export, "BoneName")?.ToString(),
                    location = Member(export, "RelativeLocation")?.ToString(),
                });
            }
        }
        catch
        {
            // Sockets are optional evidence.
        }
        return sockets;
    }

    // -- skeletal mesh -----------------------------------------------------------------------------

    static void DumpMesh(string path)
    {
        var mesh = Provider.LoadPackageObject<USkeletalMesh>(AssetPath(path));
        Provider.TryLoadPackage(path, out var package);
        var skeletonName = package != null ? ResolveName(package, mesh.Skeleton) : "";
        var boneNames = mesh.ReferenceSkeleton.FinalRefBoneInfo.Select(b => b.Name.Text).ToList();
        var materials = mesh.SkeletalMaterials.Select(m => new
        {
            slot = m.MaterialSlotName.Text,
            material = package != null ? ResolveName(package, m.MaterialInterface) : "",
        }).ToList();
        var morphs = mesh.MorphTargets.Select(m => package != null ? ResolveName(package, m) : "").ToList();
        var sockets = SocketsOf(path);
        // Which bones the vertex chunks actually reference: a bone that is merely bound but never
        // skinned cannot deform anything.
        var skinned = new List<string>();
        var lod = mesh.LODModels.FirstOrDefault();
        if (lod != null)
        {
            // UE4.18 PS4 meshes keep per-section vertices and list the bones that affect the LOD in
            // ActiveBoneIndices; chunk bone maps are a newer path and are empty here.
            var indices = lod.ActiveBoneIndices.Length > 0
                ? lod.ActiveBoneIndices.Select(i => (int)i)
                : lod.Chunks.SelectMany(chunk => (chunk.BoneMap ?? Array.Empty<ushort>()).Select(i => (int)i));
            skinned = indices.Distinct().Where(i => i >= 0 && i < boneNames.Count).Select(i => boneNames[i]).ToList();
        }

        var document = new
        {
            asset = path,
            skeleton = skeletonName,
            boneCount = boneNames.Count,
            fingerBones = boneNames.Count(IsFinger),
            skinnedBoneCount = skinned.Count,
            skinnedFingerBones = skinned.Count(IsFinger),
            skinnedFingers = skinned.Where(IsFinger).ToList(),
            lodCount = mesh.LODInfo.Length,
            materials,
            morphTargets = morphs,
            sockets,
            physicsAsset = package != null ? ResolveName(package, mesh.PhysicsAsset) : "",
            bones = boneNames,
        };
        WriteJson("mesh-" + Safe(path) + ".json", document);
        Console.WriteLine($"mesh {path}: skeleton={skeletonName} bones={boneNames.Count} " +
                          $"fingers={document.fingerBones} lods={document.lodCount} " +
                          $"materials={materials.Count} morphs={morphs.Count} sockets={sockets.Count}");
    }

    // -- animations --------------------------------------------------------------------------------

    static void DumpAnim(string path)
    {
        var package = Provider.LoadPackage(path);
        var sequence = package.GetExports().OfType<UAnimSequence>().FirstOrDefault();
        if (sequence is null) { Console.WriteLine($"{path}: no AnimSequence"); return; }
        var skeletonName = ResolveName(package, sequence.Skeleton);
        var boneNames = BoneNames(skeletonName);
        var raw = (sequence.TrackToSkeletonMapTable ?? Array.Empty<FTrackToSkeletonMap>()).Select(t => t.BoneTreeIndex).ToList();
        var compressed = (sequence.CompressedTrackToSkeletonMapTable ?? Array.Empty<FTrackToSkeletonMap>()).Select(t => t.BoneTreeIndex).ToList();
        string BoneAt(int index) => index >= 0 && index < boneNames.Count ? boneNames[index] : "?";
        var document = new
        {
            asset = path,
            skeleton = skeletonName,
            frames = sequence.NumFrames,
            rawTracks = raw.Select(i => BoneAt(i)).ToList(),
            compressedTracks = compressed.Select(i => BoneAt(i)).ToList(),
            fingerBones = raw.Concat(compressed).Select(BoneAt).Where(IsFinger).Distinct().ToList(),
            curves = (sequence.CompressedCurveNames ?? Array.Empty<CUE4Parse.UE4.Objects.Engine.Animation.FSmartName>()).Select(c => c.DisplayName.Text).ToList(),
        };
        WriteJson("anim-" + Safe(path) + ".json", document);
        Console.WriteLine($"anim {path}: frames={sequence.NumFrames} raw={raw.Count} " +
                          $"compressed={compressed.Count} finger={document.fingerBones.Count} " +
                          $"curves={document.curves.Count}");
    }

    static void ScanAnims(string filter)
    {
        var rows = new StringBuilder("asset,kind,skeleton,frames,rawTracks,compressedTracks,fingerTracks,curves\n");
        int sequences = 0, failed = 0;
        foreach (var file in Provider.Files.Values.Where(f => IsGameAsset(f, filter)).OrderBy(f => FullPath(f)))
        {
            var path = FullPath(file);
            try
            {
                var package = Provider.LoadPackage(path);
                var sequence = package.GetExports().OfType<UAnimSequence>().FirstOrDefault();
                if (sequence is null) continue;
                var skeletonName = ResolveName(package, sequence.Skeleton);
                var boneNames = BoneNames(skeletonName);
                var indices = (sequence.TrackToSkeletonMapTable ?? Array.Empty<FTrackToSkeletonMap>()).Select(t => t.BoneTreeIndex)
                    .Concat((sequence.CompressedTrackToSkeletonMapTable ?? Array.Empty<FTrackToSkeletonMap>()).Select(t => t.BoneTreeIndex))
                    .Where(i => i >= 0 && i < boneNames.Count).Distinct().ToList();
                var fingerTrackCount = indices.Count(i => IsFinger(boneNames[i]));
                rows.Append(path).Append(",sequence,").Append(skeletonName).Append(',')
                    .Append(sequence.NumFrames).Append(',')
                    .Append(sequence.TrackToSkeletonMapTable?.Length ?? 0).Append(',')
                    .Append(sequence.CompressedTrackToSkeletonMapTable?.Length ?? 0).Append(',')
                    .Append(fingerTrackCount).Append(',')
                    .Append(sequence.CompressedCurveNames?.Length ?? 0).Append('\n');
                sequences++;
            }
            catch (Exception ex)
            {
                failed++;
                Console.Error.WriteLine($"{path}: {ex.GetType().Name}: {ex.Message}");
            }
        }
        File.WriteAllText(Path.Combine(OutDir, "animations.csv"), rows.ToString());
        Console.WriteLine($"sequences={sequences} failed={failed}");
    }

    // -- pose assets -------------------------------------------------------------------------------

    // Measures how much each finger track rotates across the pose set: that is what tells an
    // articulated hand from a set of rigid wrist poses.
    static void PoseFinger(string path)
    {
        var package = Provider.LoadPackage(path);
        var export = package.GetExports().FirstOrDefault(e => e.ExportType.Contains("PoseAsset"));
        if (export is null) { Console.WriteLine($"{path}: no PoseAsset"); return; }
        var container = Member(export, "PoseContainer")!;
        var tracks = ((System.Collections.IEnumerable)Member(container, "Tracks")!).Cast<object?>()
            .Select(t => (t as FName?)?.Text ?? t?.ToString() ?? "?").ToList();
        var poses = ((System.Collections.IEnumerable)Member(container, "Poses")!).Cast<object?>().ToList();
        var poseNames = ((System.Collections.IEnumerable)Member(container, "PoseNames_DEPRECATED")!)
            .Cast<object?>().Select(p => (Member(p, "DisplayName") as FName?)?.Text ?? "?").ToList();

        var rotations = poses.Select(p => (Member(p, "LocalSpacePose") as System.Collections.IEnumerable)?
            .Cast<object?>().Select(e => e is FTransform t ? t.Rotation : default).ToList()).ToList();

        var variation = new List<object>();
        if (rotations.Count > 1 && rotations[0] != null)
        {
            for (var track = 0; track < tracks.Count && track < rotations[0]!.Count; track++)
            {
                if (!IsFinger(tracks[track])) continue;
                var max = 0.0;
                foreach (var pose in rotations.Skip(1))
                {
                    if (pose is null || track >= pose.Count) continue;
                    max = Math.Max(max, Angle(rotations[0]![track], pose[track]));
                }
                variation.Add(new { bone = tracks[track], maxDegrees = Math.Round(max * 180.0 / Math.PI, 2) });
            }
        }

        var document = new
        {
            asset = path,
            poseCount = poses.Count,
            poseNames = poseNames.Take(80).ToList(),
            tracks = tracks,
            fingerTracks = tracks.Count(IsFinger),
            fingerVariation = variation,
        };
        WriteJson("posefinger-" + Safe(path) + ".json", document);
        Console.WriteLine($"pose {path}: poses={poses.Count} tracks={tracks.Count} " +
                          $"fingerTracks={document.fingerTracks} articulated=" +
                          $"{variation.Count(v => (double)v.GetType().GetProperty("maxDegrees")!.GetValue(v)! > 1.0)}");
    }

    static double Angle(FQuat a, FQuat b)
    {
        var dot = Math.Abs((double)(a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W));
        return 2.0 * Math.Acos(Math.Clamp(dot, 0.0, 1.0));
    }
}

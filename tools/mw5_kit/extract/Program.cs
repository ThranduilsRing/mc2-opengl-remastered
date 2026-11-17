using CUE4Parse.Compression;
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.MappingsProvider;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.Animation;
using CUE4Parse.UE4.Assets.Exports.Material;
using CUE4Parse.UE4.Assets.Exports.SkeletalMesh;
using CUE4Parse.UE4.Assets.Exports.Texture;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Versions;
using CUE4Parse_Conversion.Animations;
using CUE4Parse_Conversion.Meshes;
using CUE4Parse_Conversion.Textures;
using Newtonsoft.Json;
using SkiaSharp;

namespace Mw5Extract;

public record Request(string mesh, string[]? clips, string[]? materials, string[]? loadouts, string[]? textures);
public static class Program
{
    static float[] V(FVector v) => [v.X,v.Y,v.Z];
    static float[] Q(FQuat q) => [q.X,q.Y,q.Z,q.W];
    static float[] Local(FVector value, FVector parentScale)
    {
        if (parentScale.X == 0 || parentScale.Y == 0 || parentScale.Z == 0)
            throw new InvalidDataException("Zero ancestor scale: keep on bridge");
        return [value.X/parentScale.X,value.Y/parentScale.Y,value.Z/parentScale.Z];
    }

    public static int Main(string[] args)
    {
        try
        {
            if (args is ["--fixture", var fixture]) { Native.Fixture(fixture); return 0; }
            if (args.Length < 8 || args.Length % 2 != 0)
                throw new ArgumentException("--install DIR --paks DIR --request JSON --out DIR [--ue 4.26|5.5] [--usmap FILE] [--aes HEX]");
            var a = Enumerable.Range(0,args.Length/2).ToDictionary(i=>args[2*i],i=>args[2*i+1]);
            string install = Path.GetFullPath(a["--install"]), output = Path.GetFullPath(a["--out"]);
            if (!Directory.Exists(install)) throw new DirectoryNotFoundException("Player install is missing");
            if (Inside(output, install) || Inside(install, output))
                throw new ArgumentException("Output must be separate from the read-only player install");
            for (var p = new DirectoryInfo(output); p != null; p = p.Parent)
                if (Directory.Exists(Path.Combine(p.FullName,".git")) || File.Exists(Path.Combine(p.FullName,".git")))
                    throw new ArgumentException("Never write extracted bytes in a repository");
            if (Directory.Exists(output) && Directory.EnumerateFileSystemEntries(output).Any())
                throw new ArgumentException("Output must be new or empty (prevents stale success files)");
            var paks = Path.GetFullPath(a["--paks"]);
            if (!Inside(paks,install)) throw new ArgumentException("Paks must belong to the player install");
            // Never call DownloadOodleDll: only use a runtime binary from this install.
            var dll = new[] { install, Path.Combine(install,"Engine","Binaries","ThirdParty","Oodle","Win64"),
                Path.Combine(install,"MW5Mercs","Binaries","Win64"), Path.Combine(install,"MW5Clans","Binaries","Win64") }
                .Where(Directory.Exists).SelectMany(p=>Directory.EnumerateFiles(p,"oo2core_*_win64.dll",SearchOption.TopDirectoryOnly))
                .OrderByDescending(p=>p,StringComparer.Ordinal).FirstOrDefault();
            if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("Real extraction needs Windows. Box supports synthetic fixtures only.");
            // MW5 Mercs has no Oodle DLL (Oodle is inside the exe). Paks without Oodle blocks still read;
            // Oodle-compressed assets then fail per asset and stay on the bridge. Nothing is downloaded.
            if (dll != null) OodleHelper.Initialize(dll);
            else Console.Error.WriteLine("mw5_extract: no Oodle DLL in the install; Oodle-compressed assets stay on the bridge.");
            var ue = a.GetValueOrDefault("--ue","4.26");
            var game = ue switch { "4.26"=>EGame.GAME_UE4_26, "5.5"=>EGame.GAME_UE5_5, _=>throw new ArgumentException("Unsupported UE version") };
            if (ue == "5.5" && !a.ContainsKey("--usmap")) throw new ArgumentException("UE5.5 requires the player's matching --usmap");
            using var provider = new DefaultFileProvider(paks,SearchOption.AllDirectories,new VersionContainer(game),StringComparer.OrdinalIgnoreCase);
            if (a.TryGetValue("--usmap",out var usmap)) provider.MappingsContainer = new FileUsmapTypeMappingsProvider(usmap);
            provider.Initialize(); provider.Mount();
            if (a.TryGetValue("--aes",out var aes)) provider.SubmitKey(new FGuid(),new FAesKey(aes));
            provider.PostMount();
            var request = JsonConvert.DeserializeObject<Request>(File.ReadAllText(a["--request"])) ?? throw new InvalidDataException("Empty request");
            Directory.CreateDirectory(output);
            var reader = new Reader(provider,output);
            reader.Extract(request);
            return reader.Fallbacks == 0 ? 0 : 2;
        }
        catch (Exception e) { Console.Error.WriteLine($"mw5_extract: {e.GetType().Name}: {e.Message}"); return 1; }
    }

    static bool Inside(string child,string parent) => child.Equals(parent,StringComparison.OrdinalIgnoreCase) ||
        child.StartsWith(Path.TrimEndingDirectorySeparator(parent)+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase);

    sealed class Reader(DefaultFileProvider provider,string output)
    {
        readonly List<object> bridge = [];
        readonly Dictionary<string,object> materials = new(StringComparer.Ordinal);
        readonly HashSet<string> textures = new(StringComparer.Ordinal);
        public int Fallbacks => bridge.Count;
        void Save(string name,object value) => Native.Json(Path.Combine(output,name),value);
        void Attempt(string kind,string asset,Action action)
        {
            try { action(); }
            catch (Exception e) { bridge.Add(new { kind, asset, reason=$"{e.GetType().Name}: {e.Message}", action="keep_umodel_bridge" }); }
        }

        public void Extract(Request r)
        {
            try
            {
                Attempt("skeletal_mesh",r.mesh,()=>Mesh(provider.LoadPackageObject<USkeletalMesh>(r.mesh)));
                foreach (var path in r.clips ?? []) Attempt("animation",path,()=>Animation(provider.LoadPackageObject<UAnimSequence>(path)));
                foreach (var path in r.materials ?? []) Attempt("material",path,()=>Material(provider.LoadPackageObject(path)));
                var loadouts = new List<object>();
                foreach (var path in r.loadouts ?? []) Attempt("loadout",path,()=> {
                    var obj = provider.LoadPackageObject(path);
                    if (obj.Properties.Count == 0) throw new InvalidDataException("No decoded loadout properties");
                    loadouts.Add(new { asset=path, type=obj.ExportType, properties=obj.Properties });
                });
                foreach (var path in r.textures ?? []) Attempt("texture",path,()=>Texture(provider.LoadPackageObject<UTexture2D>(path)));
                Save("materials.json",materials.Values); Save("loadout.json",loadouts);
            }
            finally { Save("bridge.json",bridge); }
            Console.WriteLine($"Native extraction complete: {bridge.Count} assets remain on bridge (see bridge.json)");
        }

        USkeletalMesh? mesh;
        void Mesh(USkeletalMesh m)
        {
            if (!m.TryConvert(out var converted) || converted.LODs.Count == 0 || m.LODModels![0].SkipLod)
                throw new InvalidDataException("LOD0 unavailable");
            var sk = m.ReferenceSkeleton;
            var bones = sk.FinalRefBoneInfo.Select((b,i)=>new Bone(b.Name.Text,b.ParentIndex,
                Local(sk.FinalRefBonePose[i].Translation,sk.GetBoneScale(sk.FinalRefBonePose,i)),
                Q(sk.FinalRefBonePose[i].Rotation),V(sk.FinalRefBonePose[i].Scale3D))).ToArray();
            var lods = new List<object>();
            var sourceLods = m.LODModels!.Select((lod,index)=>(lod,index)).Where(x=>!x.lod.SkipLod).ToArray();
            for (int i=0;i<converted.LODs.Count;i++)
            {
                var lod = converted.LODs[i];
                int sourceLod = sourceLods[i].index;
                string file = sourceLod == 0 ? "mesh.bin" : $"mesh.lod{sourceLod}.bin";
                var vertices = lod.Verts.Select(v=>new Vertex(V(v.Position),[v.Normal.X,v.Normal.Y,v.Normal.Z],
                    [v.UV.U,v.UV.V],v.Influences.Select(b=>new Influence((uint)b.Bone,b.Weight)).ToArray())).ToArray();
                var ib = lod.Indices.Value;
                uint[] indices = ib.Indices32.Length > 0 ? ib.Indices32 : ib.Indices16.Select(x=>(uint)x).ToArray();
                Native.Mesh(Path.Combine(output,file),vertices,indices);
                lods.Add(new { file, sourceLod, sections=lod.Sections.Value.Select(s=>new { material=s.MaterialIndex, firstIndex=s.FirstIndex, indexCount=s.NumFaces*3 }) });
            }
            Save("skeleton.json",new { version=1, space="UE_LH_Z_UP_CM", bones });
            Save("lods.json",lods);
            mesh = m;
            var sockets = new Dictionary<string,object>();
            var refs = m.Sockets.ToList();
            Attempt("skeleton_sockets",m.Skeleton.ToString(),()=>refs.InsertRange(0,
                (m.Skeleton.Load<USkeleton>() ?? throw new InvalidDataException("Skeleton unresolved")).Sockets));
            foreach (var index in refs) Attempt("socket",index.ToString(),()=> {
                var s = index.Load<USkeletalMeshSocket>() ?? throw new InvalidDataException("Socket unresolved");
                sockets[s.SocketName.Text] = new { name=s.SocketName.Text,bone=s.BoneName.Text,
                    translation=V(s.RelativeLocation),rotation=Q(s.RelativeRotation.Quaternion()),scale=V(s.RelativeScale) };
            });
            Save("sockets.json",sockets.Values);
            foreach (var mat in m.Materials ?? []) if (mat != null) Attempt("material",mat.ToString()!,()=>Material(
                mat.Load<UObject>() ?? throw new InvalidDataException("Material unresolved")));
        }

        void Animation(UAnimSequence a)
        {
            if (mesh == null) throw new InvalidDataException("Mesh failed; cannot bind clip");
            if (a.AdditiveAnimType != EAdditiveAnimationType.AAT_None) throw new InvalidDataException("Additive clip stays on bridge");
            var skeleton = a.Skeleton.Load<USkeleton>() ?? throw new InvalidDataException("Animation skeleton unresolved");
            var sk = skeleton.ReferenceSkeleton;
            // The skeleton asset may carry more bones (virtual/IK) than the mesh; bind clip tracks to mesh bones by name.
            var mb = mesh.ReferenceSkeleton.FinalRefBoneInfo;
            var map = Native.BindBones(mb.Select(b=>(b.Name.Text,b.ParentIndex)).ToArray(),
                sk.FinalRefBoneInfo.Select(b=>(b.Name.Text,b.ParentIndex)).ToArray());
            var clip = skeleton.ConvertAnims(a).Sequences.Single();
            if (clip.Tracks.Count != sk.FinalRefBoneInfo.Length) throw new InvalidDataException($"Missing bone tracks ({clip.Tracks.Count} vs {sk.FinalRefBoneInfo.Length})");
            var pose = clip.RetargetBasePose ?? sk.FinalRefBonePose;
            var tracks = Enumerable.Range(0,mb.Length).Select(i=>{ var t = clip.Tracks[map[i]]; return new Track(i,
                Native.Keys(t.KeyPos.Select(v=>Local(v,AnimationScale(sk.GetBoneScale(pose,map[i])))).ToArray(),t.KeyPosTime.Length>0?t.KeyPosTime:t.KeyTime,clip.FramesPerSecond,clip.NumFrames),
                Native.Keys(t.KeyQuat.Select(Q).ToArray(),t.KeyQuatTime.Length>0?t.KeyQuatTime:t.KeyTime,clip.FramesPerSecond,clip.NumFrames),
                !AuthoredScale(a,map[i]) ? new Channel([],[]) : // CUE4Parse reports a zero key for tracks without authored scale
                Native.Keys(t.KeyScale.Select(V).ToArray(),t.KeyScaleTime.Length>0?t.KeyScaleTime:t.KeyTime,clip.FramesPerSecond,clip.NumFrames)); }).ToArray();
            Directory.CreateDirectory(Path.Combine(output,"clips"));
            Native.Animation(Path.Combine(output,"clips",Native.Id(a.GetPathName())+".bin"),new(clip.NumFrames,clip.FramesPerSecond,tracks));
            Native.Json(Path.Combine(output,"clips",Native.Id(a.GetPathName())+".json"),new { asset=a.GetPathName(), name=a.Name });
        }

        // True when the compressed data holds scale keys for this skeleton bone (offset != INDEX_NONE), so an
        // authored zero scale is kept and only tracks without scale are dropped. Other codecs stay on the bridge.
        static bool AuthoredScale(UAnimSequence a,int bone)
        {
            if (a.CompressedDataStructure is not FUECompressedAnimData u) throw new InvalidDataException("Unsupported animation codec: keep on bridge");
            int track = Array.FindIndex(a.CompressedTrackToSkeletonMapTable,m=>m.BoneTreeIndex == bone);
            var o = u.CompressedScaleOffsets;
            return Native.HasScale(o.OffsetData,o.StripSize,track);
        }

        static FVector AnimationScale(FVector scale) =>
            Math.Abs(scale.X-1) > .001f || Math.Abs(scale.Y-1) > .001f || Math.Abs(scale.Z-1) > .001f ? scale : new FVector(1);

        void Material(UObject obj)
        {
            string path = obj.GetPathName();
            if (materials.ContainsKey(path)) return;
            if (obj is not UMaterialInstanceConstant m) throw new InvalidDataException("Only material instances decoded; base material stays on bridge");
            // Retain local overrides and parent chain, including MTI/SKN names and static switches.
            materials[path] = new { asset=path, parent=m.Parent?.GetPathName(), scalars=m.ScalarParameterValues,
                vectors=m.VectorParameterValues, textures=m.TextureParameterValues.Select(t=>new { name=t.Name, asset=t.ParameterValue.ResolvedObject?.GetPathName() }),
                switches=m.StaticParameters, overrides=m.BasePropertyOverrides, properties=m.Properties };
            if (m.Parent != null) Attempt("material_parent",m.Parent.GetPathName(),()=>Material(m.Parent));
            foreach (var t in m.TextureParameterValues) Attempt("texture",t.ParameterValue.ToString(),()=>Texture(
                t.ParameterValue.Load<UTexture2D>() ?? throw new InvalidDataException("Texture unresolved")));
        }

        void Texture(UTexture2D t)
        {
            string asset=t.GetPathName();
            if (textures.Contains(asset)) return;
            using var bitmap=t.Decode() ?? throw new InvalidDataException("Texture decoder returned no pixels");
            using var data=bitmap.Encode(SKEncodedImageFormat.Png,100);
            if (data == null) throw new InvalidDataException("PNG encoder failed");
            Directory.CreateDirectory(Path.Combine(output,"textures"));
            using (var stream=File.Create(Path.Combine(output,"textures",Native.Id(asset)+".png"))) data.SaveTo(stream);
            textures.Add(asset);
            Save("textures.json",textures.Select(p=>new { asset=p, file="textures/"+Native.Id(p)+".png" }));
        }
    }
}

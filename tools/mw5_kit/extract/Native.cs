using System.Text;
using Newtonsoft.Json;

namespace Mw5Extract;

// All transforms are local UE coordinates, centimeters, quaternion XYZW.
public record Bone(string name, int parent, float[] translation, float[] rotation, float[] scale);
public record Influence(uint bone, float weight);
public record Vertex(float[] position, float[] normal, float[] uv, Influence[] influences);
public record Channel(float[] times, float[][] values);
public record Track(int bone, Channel translation, Channel rotation, Channel scale);
public record Clip(int frames, float fps, Track[] tracks);

public static class Native
{
    // Maps each mesh bone to its index in the (possibly larger) clip skeleton by name; parents must agree.
    public static int[] BindBones((string name, int parent)[] mesh, (string name, int parent)[] skeleton)
    {
        var index = new Dictionary<string, int>(StringComparer.Ordinal);
        for (int i = 0; i < skeleton.Length; i++) index[skeleton[i].name] = i;
        var map = new int[mesh.Length];
        for (int i = 0; i < mesh.Length; i++)
        {
            if (!index.TryGetValue(mesh[i].name, out map[i])) throw new InvalidDataException($"Mesh bone {mesh[i].name} missing from clip skeleton");
            string? want = mesh[i].parent < 0 ? null : mesh[mesh[i].parent].name;
            int p = skeleton[map[i]].parent;
            if (want != (p < 0 ? null : skeleton[p].name)) throw new InvalidDataException($"Bone parent differs: {mesh[i].name}");
        }
        return map;
    }

    // UE compressed scale offsets: one (offset, count) strip per compressed track; offset -1 = no scale keys.
    public static bool HasScale(int[]? offsets, int stripSize, int track) =>
        track >= 0 && offsets is { Length: > 0 } && (track + 1) * stripSize <= offsets.Length && offsets[track * stripSize] >= 0;

    public static void Json(string path, object value) =>
        File.WriteAllText(path, JsonConvert.SerializeObject(value, Formatting.Indented));

    public static void Mesh(string path, Vertex[] vertices, uint[] indices)
    {
        if (indices.Length % 3 != 0 || indices.Any(i => i >= vertices.Length))
            throw new InvalidDataException("Invalid triangle indices");
        using var w = new BinaryWriter(File.Create(path));
        w.Write(Encoding.ASCII.GetBytes("MW5M")); w.Write(1);
        w.Write(vertices.Length); w.Write(indices.Length);
        foreach (var v in vertices)
        {
            Floats(w, v.position, 3); Floats(w, v.normal, 3); Floats(w, v.uv, 2);
            w.Write(v.influences.Length);
            foreach (var i in v.influences) { w.Write(i.bone); w.Write(i.weight); }
        }
        foreach (var i in indices) w.Write(i);
    }

    public static void Animation(string path, Clip clip)
    {
        using var w = new BinaryWriter(File.Create(path));
        w.Write(Encoding.ASCII.GetBytes("MW5A")); w.Write(1);
        w.Write(clip.frames); w.Write(clip.fps); w.Write(clip.tracks.Length);
        foreach (var t in clip.tracks)
        {
            w.Write(t.bone);
            WriteChannel(w, t.translation, 3); WriteChannel(w, t.rotation, 4); WriteChannel(w, t.scale, 3);
        }
    }

    static void WriteChannel(BinaryWriter w, Channel c, int width)
    {
        if (c.times.Length != c.values.Length) throw new InvalidDataException("Key/time mismatch");
        w.Write(c.times.Length);
        for (int i = 0; i < c.times.Length; i++)
        { w.Write(c.times[i]); Floats(w, c.values[i], width); }
    }

    static void Floats(BinaryWriter w, float[] values, int width)
    {
        if (values.Length != width || values.Any(x => !float.IsFinite(x)))
            throw new InvalidDataException("Invalid native vector");
        foreach (var x in values) w.Write(x);
    }

    public static string Id(string path) => Convert.ToHexString(
        System.Security.Cryptography.SHA256.HashData(Encoding.UTF8.GetBytes(path))).ToLowerInvariant();

    // Shared timing conversion: CUE tracks contain frame positions, not seconds.
    public static Channel Keys(float[][] values, float[] times, float fps, int frames = 0)
    {
        if (!(fps > 0) || !float.IsFinite(fps)) throw new InvalidDataException("Invalid frame rate");
        if (values.Length > 1 && times.Length != 0 && times.Length != values.Length)
            throw new InvalidDataException("Unsupported shared time array");
        return new Channel(Enumerable.Range(0, values.Length)
            .Select(i => values.Length == 1 ? 0 : (times.Length == 0 ?
                i * (frames > 0 ? (float)frames / values.Length : 1) : times[i]) / fps).ToArray(), values);
    }

    // Synthetic fixture exercises the production writers, no game data needed.
    public static void Fixture(string dir)
    {
        Directory.CreateDirectory(Path.Combine(dir, "clips"));
        Json(Path.Combine(dir, "skeleton.json"), new { version = 1, space = "UE_LH_Z_UP_CM", bones = new[] {
            new Bone("root", -1, [100,200,300], [0,0,0,1], [1,1,1]),
            new Bone("weapon", 0, [0,0,100], [0,0,0,1], [1,1,1]) } });
        Mesh(Path.Combine(dir, "mesh.bin"), [
            new([0,0,0], [0,0,1], [0,0], [new(0,1)]),
            new([100,0,0], [0,0,1], [1,0], [new(0,.25f),new(1,.75f)]),
            new([0,100,0], [0,0,1], [0,1], [new(1,1)])], [0,1,2]);
        var empty = new Channel([], []);
        Animation(Path.Combine(dir, "clips", "turn.bin"), new(3, 2, [new(1,
            Keys([[0,0,100],[0,0,200]], [0,2], 2),
            Keys([[0,0,0,1],[0,0,.70710677f,.70710677f]], [0,2], 2), empty)]));
        Json(Path.Combine(dir, "sockets.json"), new[] { new {
            name="muzzle", bone="weapon", translation=new[]{0f,0,50}, rotation=new[]{0f,0,0,1}, scale=new[]{1f,1,1} } });
        Json(Path.Combine(dir, "materials.json"), Array.Empty<object>());
        Json(Path.Combine(dir, "loadout.json"), Array.Empty<object>());
        Json(Path.Combine(dir, "bridge.json"), Array.Empty<object>());
        if (Keys([[1,2,3]], [17,21], 30).times[0] != 0) throw new Exception("constant time");
        if (Math.Abs(Keys([[1,2,3],[4,5,6]], [], 30, 6).times[1]-.1f) > 1e-6f)
            throw new Exception("uniform sparse time");
        try { Keys([[1,2,3],[4,5,6]], [1], 30); throw new Exception("bad timing accepted"); }
        catch (InvalidDataException) { }
        var mesh = new[] { ("root", -1), ("a", 0) };
        var wide = new[] { ("root", -1), ("virtual", 0), ("a", 0), ("b", 2) };
        if (!BindBones(mesh, wide).SequenceEqual([0, 2])) throw new Exception("bone remap by name");
        foreach (var bad in new[] { new[] { ("root", -1), ("a", 1) }, new[] { ("root", -1), ("b", 0) } })
            try { BindBones(bad, wide); throw new Exception("bad bone binding accepted"); } catch (InvalidDataException) { }
        if (HasScale([-1, 0, 4, 2], 2, 0) || !HasScale([-1, 0, 4, 2], 2, 1) || HasScale([-1, 0], 2, 1) || HasScale(null, 2, 0) || HasScale([4, 2], 2, -1))
            throw new Exception("authored vs absent scale");
        Console.WriteLine("PASS native writers and key timing; synthetic fixture emitted");
    }
}

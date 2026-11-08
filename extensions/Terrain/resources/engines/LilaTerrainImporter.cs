// Builds Unity terrains from a Lila terrain export (terrainExport target='unity'). Put this file in an Editor folder
// of your project, then Tools > Lila > Import Terrain and pick the export's .json. Headless:
//   Unity -batchmode -quit -projectPath <project> -executeMethod LilaTerrainImporter.ImportFromCommandLine -lilaTerrain <file.json>
// Each tile becomes a TerrainData asset and a Terrain object, neighbours connected; layers become TerrainLayer assets
// (assign their textures afterwards; existing ones of the same name are reused).

#if UNITY_EDITOR
using System;
using System.IO;
using UnityEditor;
using UnityEngine;

public static class LilaTerrainImporter
{
    [Serializable]
    class Layer
    {
        public string name;
        public string mask;
    }

    [Serializable]
    class Tile
    {
        public int x;
        public int y;
        public string heightmap;
        public string splatmap;
        public float[] position;
    }

    [Serializable]
    class Settings
    {
        public string target;
        public string name;
        public int heightmapResolution;
        public int alphamapResolution;
        public int[] tileCount;
        public float[] tileSize;
        public Layer[] layers;
        public Tile[] tiles;
    }

    [MenuItem("Tools/Lila/Import Terrain...")]
    static void ImportFromMenu()
    {
        string path = EditorUtility.OpenFilePanel("Lila terrain export", "", "json");
        if (!string.IsNullOrEmpty(path))
            Import(path);
    }

    public static void ImportFromCommandLine()
    {
        string[] args = Environment.GetCommandLineArgs();
        for (int i = 0; i + 1 < args.Length; ++i)
            if (args[i] == "-lilaTerrain")
            {
                Import(args[i + 1]);
                return;
            }
        Debug.LogError("LilaTerrainImporter: pass -lilaTerrain <export .json>");
    }

    /// <summary>Builds the terrains; returns their root object.</summary>
    public static GameObject Import(string jsonPath)
    {
        Settings s = JsonUtility.FromJson<Settings>(File.ReadAllText(jsonPath));
        if (s == null || s.target != "unity")
            throw new Exception(jsonPath + " isn't a Lila Unity terrain export");
        int res = s.heightmapResolution;
        if (res < 33 || res > 4097 || ((res - 1) & (res - 2)) != 0)
            throw new Exception("heightmapResolution " + res + " isn't 2^n + 1 from 33 to 4097");
        string folder = Path.GetDirectoryName(jsonPath);
        string assets = "Assets/LilaTerrain/" + s.name;
        Directory.CreateDirectory(assets);
        AssetDatabase.Refresh();

        TerrainLayer[] layers = new TerrainLayer[s.layers != null ? s.layers.Length : 0];
        for (int l = 0; l < layers.Length; ++l)
        {
            string layerPath = assets + "/" + s.layers[l].name + ".terrainlayer";
            layers[l] = AssetDatabase.LoadAssetAtPath<TerrainLayer>(layerPath);
            if (layers[l] == null)
            {
                layers[l] = new TerrainLayer { name = s.layers[l].name, tileSize = new Vector2(4, 4) };
                AssetDatabase.CreateAsset(layers[l], layerPath);
            }
        }

        GameObject root = new GameObject(s.name);
        Undo.RegisterCreatedObjectUndo(root, "Import Lila Terrain");
        Terrain[,] grid = new Terrain[s.tileCount[0], s.tileCount[1]];
        foreach (Tile t in s.tiles)
        {
            TerrainData data = new TerrainData();
            // Resolution before size: setting the resolution rescales the size.
            data.heightmapResolution = res;
            data.size = new Vector3(s.tileSize[0], s.tileSize[1], s.tileSize[2]);
            string tileName = s.name + "_x" + t.x + "_y" + t.y;
            // An asset before its layers: Unity saves the alpha maps only of a TerrainData that is already one.
            AssetDatabase.CreateAsset(data, assets + "/" + tileName + ".asset");
            data.SetHeights(0, 0, ReadHeights(Path.Combine(folder, t.heightmap), res));
            if (layers.Length > 0 && !string.IsNullOrEmpty(t.splatmap))
            {
                data.alphamapResolution = s.alphamapResolution;
                data.terrainLayers = layers;
                data.SetAlphamaps(0, 0, ReadSplat(Path.Combine(folder, t.splatmap), s.alphamapResolution, layers.Length));
            }
            EditorUtility.SetDirty(data);
            GameObject go = Terrain.CreateTerrainGameObject(data);
            go.name = tileName;
            go.transform.SetParent(root.transform, false);
            go.transform.position = new Vector3(t.position[0], t.position[1], t.position[2]);
            Terrain terrain = go.GetComponent<Terrain>();
            terrain.allowAutoConnect = true;
            grid[t.x, t.y] = terrain;
        }
        for (int x = 0; x < s.tileCount[0]; ++x)
            for (int y = 0; y < s.tileCount[1]; ++y)
                if (grid[x, y] != null)
                    grid[x, y].SetNeighbors(x > 0 ? grid[x - 1, y] : null, y + 1 < s.tileCount[1] ? grid[x, y + 1] : null,
                                            x + 1 < s.tileCount[0] ? grid[x + 1, y] : null, y > 0 ? grid[x, y - 1] : null);
        AssetDatabase.SaveAssets();
        Debug.Log("LilaTerrainImporter: " + s.tiles.Length + " terrain tiles from " + jsonPath);
        return root;
    }

    /// <summary>16-bit little-endian, rows along +Z: heights[row, column] as SetHeights wants them.</summary>
    static float[,] ReadHeights(string path, int res)
    {
        byte[] bytes = File.ReadAllBytes(path);
        if (bytes.Length != res * res * 2)
            throw new Exception(path + " holds " + bytes.Length + " bytes, " + res + " x " + res + " 16-bit needs " + (res * res * 2));
        float[,] heights = new float[res, res];
        for (int r = 0; r < res; ++r)
            for (int c = 0; c < res; ++c)
            {
                int i = (r * res + c) * 2;
                heights[r, c] = (bytes[i] | (bytes[i + 1] << 8)) / 65535f;
            }
        return heights;
    }

    /// <summary>8-bit weights, [row][column][layer].</summary>
    static float[,,] ReadSplat(string path, int res, int layers)
    {
        byte[] bytes = File.ReadAllBytes(path);
        if (bytes.Length != res * res * layers)
            throw new Exception(path + " holds " + bytes.Length + " bytes, " + res + " x " + res + " x " + layers + " layers needs " + (res * res * layers));
        float[,,] maps = new float[res, res, layers];
        for (int r = 0; r < res; ++r)
            for (int c = 0; c < res; ++c)
                for (int l = 0; l < layers; ++l)
                    maps[r, c, l] = bytes[(r * res + c) * layers + l] / 255f;
        return maps;
    }
}
#endif

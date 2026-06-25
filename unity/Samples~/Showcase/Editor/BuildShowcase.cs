// SPDX-License-Identifier: Apache-2.0
// Editor build helper for the Showcase sample. Invoke headless, e.g.:
//   Unity -batchmode -nographics -projectPath . -quit \
//     -executeMethod BuildShowcase.OSX -uavOut Build/showcase-osx/Showcase.app
using System;
using System.IO;
using System.Collections.Generic;
using UnityEditor;
using UnityEditor.Build.Reporting;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnitedAV.Samples.Showcase;

public static class BuildShowcase
{
    public static void OSX()     => Build(BuildTarget.StandaloneOSX,       "Build/showcase-osx/Showcase.app");
    public static void Linux()   => Build(BuildTarget.StandaloneLinux64,   "Build/showcase-linux/Showcase");
    public static void Windows() => Build(BuildTarget.StandaloneWindows64, "Build/showcase-win/Showcase.exe");

    static string Arg(string name, string def)
    {
        var a = Environment.GetCommandLineArgs();
        for (int i = 0; i < a.Length - 1; i++) if (a[i] == name) return a[i + 1];
        return def;
    }

    // Keep a built-in shader from being stripped: add it to GraphicsSettings
    // "Always Included Shaders" (the showcase creates materials via Shader.Find).
    static void EnsureAlwaysIncluded(params string[] shaderNames)
    {
        var objs = AssetDatabase.LoadAllAssetsAtPath("ProjectSettings/GraphicsSettings.asset");
        if (objs == null || objs.Length == 0) { Debug.LogError("[uav-showcase-build] cannot load GraphicsSettings"); return; }
        var so = new SerializedObject(objs[0]);
        var arr = so.FindProperty("m_AlwaysIncludedShaders");
        if (arr == null) { Debug.LogError("[uav-showcase-build] no m_AlwaysIncludedShaders"); return; }
        var existing = new HashSet<Shader>();
        for (int i = 0; i < arr.arraySize; i++)
            if (arr.GetArrayElementAtIndex(i).objectReferenceValue is Shader sh) existing.Add(sh);
        foreach (var name in shaderNames)
        {
            var sh = Shader.Find(name);
            if (sh == null || existing.Contains(sh)) continue;
            int idx = arr.arraySize;
            arr.InsertArrayElementAtIndex(idx);
            arr.GetArrayElementAtIndex(idx).objectReferenceValue = sh;
            existing.Add(sh);
            Debug.Log("[uav-showcase-build] added Always-Included shader: " + name);
        }
        so.ApplyModifiedProperties();
        AssetDatabase.SaveAssets();
    }

    static void Build(BuildTarget target, string defaultOut)
    {
        string outPath = Arg("-uavOut", defaultOut);
        EnsureAlwaysIncluded("Unlit/Texture", "Sprites/Default");

        var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        new GameObject("Showcase").AddComponent<ShowcaseController>();
        string sceneDir = "Assets/_ShowcaseScene";
        Directory.CreateDirectory(sceneDir);
        string scenePath = sceneDir + "/Showcase.unity";
        EditorSceneManager.SaveScene(scene, scenePath);
        AssetDatabase.SaveAssets();
        AssetDatabase.Refresh();

        Directory.CreateDirectory(Path.GetDirectoryName(outPath));
        var report = BuildPipeline.BuildPlayer(new BuildPlayerOptions
        {
            scenes = new[] { scenePath },
            locationPathName = outPath,
            target = target,
            targetGroup = BuildTargetGroup.Standalone,
            options = BuildOptions.None,
        });
        var s = report.summary;
        Debug.Log($"[uav-showcase-build] target={target} result={s.result} size={s.totalSize} out={s.outputPath} errors={s.totalErrors}");
        EditorApplication.Exit(s.result == BuildResult.Succeeded ? 0 : 2);
    }
}

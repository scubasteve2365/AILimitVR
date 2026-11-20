// AI Limit VR feasibility probe (not build 1). Read-only logging, then two HDRP eye cameras behind live commands.
// Commands (one per line in <game>\vrlab_command.txt): dump, eyes on|off, sep <m>, size <w> <h>, snap <name>,
// fps <n> (targetFrameRate, VSync off), vsync <n>, main on|off, ctrl (both eyes at the game's own matrices).
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using BepInEx;
using BepInEx.Logging;
using BepInEx.Unity.IL2CPP;
using Il2CppInterop.Runtime;
using Il2CppInterop.Runtime.Injection;
using UnityEngine;
using UnityEngine.Events;
using UnityEngine.Rendering;
using UnityEngine.Rendering.HighDefinition;

namespace AILimitVR
{
    [BepInPlugin("scubasteve2365.ailimitvr", "AILimitVR", "0.1.0")]
    public class Plugin : BasePlugin
    {
        public static ManualLogSource L;
        public override void Load()
        {
            L = Log;
            L.LogInfo("AILVR probe 0.1 loading");
            AddComponent<Probe>();
        }
    }

    public class Probe : MonoBehaviour
    {
        public Probe(IntPtr p) : base(p) { }
        static ManualLogSource L => Plugin.L;

        readonly Stopwatch sw = Stopwatch.StartNew();
        string cmdPath;
        double nextCmd, winStart; float winGame; int winFrames, winFixed; double winMax, last;
        int eyesOn; float sep = 0.064f; int ew = 2000, eh = 2000; bool ctrl;
        Camera eyeL, eyeR; RenderTexture rtL, rtR; Camera main;
        string snapName; int snapAtFrame = -1, eyeFrame = -1; int lastFps = -999, lastVs = -999;
        int ctxCalls, camMoves, camFrames, plMoves; Vector3 lastPl; Transform plT; Vector3 lastCamPos; Quaternion lastCamRot;

        void Start()
        {
            cmdPath = Path.Combine(Paths.GameRootPath, "vrlab_command.txt");
            try { File.WriteAllText(cmdPath, ""); } catch { }
            try
            {
                RenderPipelineManager.add_beginContextRendering(DelegateSupport.ConvertDelegate<Il2CppSystem.Action<ScriptableRenderContext, Il2CppSystem.Collections.Generic.List<Camera>>>(
                    new Action<ScriptableRenderContext, Il2CppSystem.Collections.Generic.List<Camera>>((ctx, list) => { ctxCalls++; OnBeforeRender(); })));
                L.LogInfo("HOOK beginContextRendering in place");
            }
            catch (Exception e) { L.LogError($"HOOK beginContextRendering failed: {e.Message}"); }
            L.LogInfo($"AILVR start: Unity {Application.unityVersion}, gfx {SystemInfo.graphicsDeviceType} {SystemInfo.graphicsDeviceVersion}, threaded {SystemInfo.renderingThreadingMode}");
            L.LogInfo($"AILVR pipeline {GraphicsSettings.currentRenderPipeline?.name}, colorspace {QualitySettings.activeColorSpace}, quality level {QualitySettings.GetQualityLevel()}");
            winStart = sw.Elapsed.TotalSeconds; winGame = Time.time; last = winStart;
        }

        void FixedUpdate() { winFixed++; }

        void Update()
        {
            double now = sw.Elapsed.TotalSeconds;
            double dt = now - last; last = now; winFrames++; if (dt > winMax) winMax = dt;
            if (MainCam() != null) { camFrames++; }
            if (now >= nextCmd) { nextCmd = now + 0.5; ReadCommands(); }
            if (Application.targetFrameRate != lastFps || QualitySettings.vSyncCount != lastVs)
            {
                L.LogInfo($"GAMESET targetFrameRate {Application.targetFrameRate} vSyncCount {QualitySettings.vSyncCount} (frame {Time.frameCount})");
                lastFps = Application.targetFrameRate; lastVs = QualitySettings.vSyncCount;
            }
            if (now - winStart >= 5.0)
            {
                double real = now - winStart; float game = Time.time - winGame;
                L.LogInfo($"PERF {winFrames / real:F1} fps, frame avg {1000 * real / winFrames:F2} ms max {winMax * 1000:F2} ms | SPEED game {game / real:F3} (timeScale {Time.timeScale:F3}) fixed {winFixed / real:F1}/s step {Time.fixedDeltaTime * 1000:F2} ms maxStep {Time.maximumDeltaTime * 1000:F1} ms | eyes {eyesOn} {ew}x{eh} sep {sep} | cam moved in {camMoves} of {camFrames} renders, player in {plMoves}, ctx calls {ctxCalls}");
                camMoves = camFrames = 0; ctxCalls = 0; plMoves = 0;
                winStart = now; winGame = Time.time; winFrames = 0; winFixed = 0; winMax = 0;
            }
        }

        void ReadCommands()
        {
            string[] lines;
            try { if (!File.Exists(cmdPath)) return; lines = File.ReadAllLines(cmdPath); if (lines.Length == 0) return; File.WriteAllText(cmdPath, ""); }
            catch { return; }
            foreach (var raw in lines)
            {
                var a = raw.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (a.Length == 0) continue;
                L.LogInfo($"CMD {raw.Trim()}");
                try
                {
                    switch (a[0])
                    {
                        case "dump": Dump(); break;
                        case "eyes": SetEyes(a[1] == "on"); break;
                        case "sep": sep = float.Parse(a[1]); break;
                        case "ctrl": ctrl = a.Length < 2 || a[1] == "on"; break;
                        case "size": ew = int.Parse(a[1]); eh = int.Parse(a[2]); if (eyesOn == 1) { SetEyes(false); SetEyes(true); } break;
                        case "snap": snapName = a[1]; snapAtFrame = Time.frameCount + 2; break;
                        case "fps": Application.targetFrameRate = int.Parse(a[1]); QualitySettings.vSyncCount = 0; break;
                        case "vsync": QualitySettings.vSyncCount = int.Parse(a[1]); break;
                        case "main": if (MainCam() != null) main.enabled = a[1] == "on"; break;
                        case "aa": foreach (var c in new[] { eyeL, eyeR }) if (c != null) c.GetComponent<HDAdditionalCameraData>().antialiasing = (HDAdditionalCameraData.AntialiasingMode)int.Parse(a[1]); break;
                        case "comp":
                            foreach (var o in UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Volume>()))
                            {
                                var pr = o.Cast<Volume>().sharedProfile; if (pr == null) continue;
                                foreach (var c in pr.components) if (c.GetIl2CppType().Name == a[1]) { c.active = a[2] == "on"; L.LogInfo($"COMP {a[1]} {a[2]} in {pr.name}"); }
                            }
                            break;
                        case "hud": Hud(); break;
                        case "timescale": L.LogInfo($"timeScale {Time.timeScale}"); break;
                    }
                }
                catch (Exception e) { L.LogError($"CMD failed: {e}"); }
            }
        }

        Camera MainCam()
        {
            if (main != null && main.gameObject != null) return main;
            var cm = CameraManager.Instance;
            if (cm != null) main = cm.GetRealCameraScript();
            if (main == null) main = Camera.main;
            return main;
        }

        void Dump()
        {
            var sb = new StringBuilder();
            var cm = CameraManager.Instance;
            sb.AppendLine($"DUMP frame {Time.frameCount}, CameraManager {(cm != null)}");
            if (cm != null)
            {
                var rc = cm.GetRealCameraScript(); var uc = cm.GetUICameraScript();
                sb.AppendLine($"  RealCamera {rc?.name} UICamera {uc?.name} FreeCam {cm.GetFreeCameraTransform()?.name} nowDistance {cm.nowDistance}");
            }
            var cams = Camera.allCameras;
            sb.AppendLine($"  allCameras {cams.Length}, main {Camera.main?.name}");
            foreach (var c in cams) DumpCam(sb, c);
            foreach (var o in UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Canvas>()))
            {
                var cv = o.Cast<Canvas>();
                if (!cv.isRootCanvas) continue;
                sb.AppendLine($"  CANVAS {cv.name} mode {cv.renderMode} cam {cv.worldCamera?.name} order {cv.sortingOrder} enabled {cv.enabled} active {cv.gameObject.activeInHierarchy} scale {cv.scaleFactor} layer {cv.gameObject.layer}");
            }
            foreach (var o in UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Volume>()))
            {
                var v = o.Cast<Volume>();
                var p = v.sharedProfile;
                var comps = new StringBuilder();
                if (p != null) foreach (var c in p.components) comps.Append($"{c.GetIl2CppType().Name}{(c.active ? "" : "(off)")} ");
                sb.AppendLine($"  VOLUME {v.name} global {v.isGlobal} weight {v.weight} prio {v.priority} enabled {v.enabled} active {v.gameObject.activeInHierarchy} layer {v.gameObject.layer} profile {p?.name}: {comps}");
            }
            sb.AppendLine($"  TIME fixed {Time.fixedDeltaTime} max {Time.maximumDeltaTime} timeScale {Time.timeScale} targetFrameRate {Application.targetFrameRate} vSync {QualitySettings.vSyncCount}");
            sb.AppendLine($"  SCREEN {Screen.width}x{Screen.height} fullscreen {Screen.fullScreenMode} refresh {Screen.currentResolution.refreshRateRatio.value}");
            L.LogInfo(sb.ToString());
        }

        void DumpCam(StringBuilder sb, Camera c)
        {
            var hd = c.GetComponent<HDAdditionalCameraData>();
            var t = c.transform;
            sb.AppendLine($"  CAM {c.name} enabled {c.enabled} depth {c.depth} fov {c.fieldOfView:F2} near {c.nearClipPlane} far {c.farClipPlane} mask 0x{c.cullingMask:X8} target {c.targetTexture?.name} pix {c.pixelWidth}x{c.pixelHeight} pos {t.position} rot {t.eulerAngles} dynres {c.allowDynamicResolution}");
            if (hd != null)
                sb.AppendLine($"     HD aa {hd.antialiasing} dlss {hd.allowDeepLearningSuperSampling} dlssQ {hd.deepLearningSuperSamplingQuality} customDLSS {hd.deepLearningSuperSamplingUseCustomQualitySettings} dynres {hd.allowDynamicResolution} customFS {hd.customRenderingSettings} clear {hd.clearColorMode} volumeMask 0x{hd.volumeLayerMask.value:X8} anchor {hd.volumeAnchorOverride?.name} flipY {hd.flipYMode} xr {hd.xrRendering}");
        }

        void SetEyes(bool on)
        {
            if (!on)
            {
                if (eyeL != null) { UnityEngine.Object.Destroy(eyeL.gameObject); UnityEngine.Object.Destroy(eyeR.gameObject); }
                if (rtL != null) { rtL.Release(); rtR.Release(); }
                eyeL = eyeR = null; rtL = rtR = null; eyesOn = 0; L.LogInfo("EYES off"); return;
            }
            if (MainCam() == null) { L.LogWarning("EYES no game camera yet"); return; }
            rtL = MakeRT("AILVR_L"); rtR = MakeRT("AILVR_R");
            eyeL = MakeEye("AILVR_EyeL", rtL); eyeR = MakeEye("AILVR_EyeR", rtR);
            eyesOn = 1;
            var sb = new StringBuilder(); DumpCam(sb, eyeL); DumpCam(sb, eyeR);
            L.LogInfo($"EYES on {ew}x{eh} format {rtL.format} sRGB {rtL.sRGB}\n{sb}");
        }

        Camera hudCam; RenderTexture hudRT;
        void Hud()
        {
            if (hudCam != null) { hudSnapAt = Time.frameCount + 2; return; }
            var asset = GraphicsSettings.currentRenderPipeline.TryCast<HDRenderPipelineAsset>();
            if (asset != null) L.LogInfo($"HDRP colorBufferFormat {asset.currentPlatformRenderPipelineSettings.colorBufferFormat}");
            hudRT = new RenderTexture(1920, 1080, 24, RenderTextureFormat.ARGB32, RenderTextureReadWrite.sRGB) { name = "AILVR_HUD" }; hudRT.Create();
            var go = new GameObject("AILVR_HudCam"); UnityEngine.Object.DontDestroyOnLoad(go);
            hudCam = go.AddComponent<Camera>();
            hudCam.clearFlags = CameraClearFlags.SolidColor; hudCam.backgroundColor = new Color(0, 0, 0, 0);
            hudCam.cullingMask = 1 << 5; hudCam.depth = 10; hudCam.targetTexture = hudRT;
            var hd = go.AddComponent<HDAdditionalCameraData>();
            hd.clearColorMode = HDAdditionalCameraData.ClearColorMode.Color; hd.backgroundColorHDR = new Color(0, 0, 0, 0);
            hd.volumeLayerMask = 0; hd.antialiasing = HDAdditionalCameraData.AntialiasingMode.None;
            int n = 0;
            foreach (var o in UnityEngine.Object.FindObjectsOfType(Il2CppType.Of<Canvas>()))
            {
                var cv = o.Cast<Canvas>();
                if (!cv.isRootCanvas || cv.renderMode != RenderMode.ScreenSpaceOverlay) continue;
                cv.renderMode = RenderMode.ScreenSpaceCamera; cv.worldCamera = hudCam; cv.planeDistance = 1; n++;
            }
            L.LogInfo($"HUD {n} overlay canvases moved to AILVR_HudCam");
            eyeFrame = Time.frameCount; snapName = "hud"; hudSnapAt = Time.frameCount + 3;
        }
        int hudSnapAt = -1;
        void LateUpdate()
        {
            if (hudSnapAt < 0 || Time.frameCount < hudSnapAt) return;
            hudSnapAt = -1;
            string dir = Path.Combine(@"C:\VRLab\evidence\AILimitVR", DateTime.Now.ToString("yyyy-MM-dd"), "eyes");
            var prev = RenderTexture.active; RenderTexture.active = hudRT;
            var tex = new Texture2D(hudRT.width, hudRT.height, TextureFormat.RGBA32, false);
            tex.ReadPixels(new Rect(0, 0, hudRT.width, hudRT.height), 0, 0); tex.Apply(); RenderTexture.active = prev;
            File.WriteAllBytes(Path.Combine(dir, $"hud_f{Time.frameCount}.png"), ImageConversion.EncodeToPNG(tex));
            L.LogInfo($"SNAP hud f{Time.frameCount}");
        }

        RenderTexture MakeRT(string n)
        {
            var rt = new RenderTexture(ew, eh, 24, RenderTextureFormat.ARGB32, RenderTextureReadWrite.sRGB) { name = n };
            rt.Create(); return rt;
        }

        Camera MakeEye(string n, RenderTexture rt)
        {
            var go = new GameObject(n); UnityEngine.Object.DontDestroyOnLoad(go);
            var c = go.AddComponent<Camera>();
            c.CopyFrom(main);
            var hd = go.AddComponent<HDAdditionalCameraData>();
            var mhd = main.GetComponent<HDAdditionalCameraData>();
            if (mhd != null) mhd.CopyTo(hd);
            c.targetTexture = rt; c.depth = main.depth - 1; c.enabled = true;
            c.transform.SetPositionAndRotation(main.transform.position, main.transform.rotation);
            return c;
        }

        // Runs after every LateUpdate, before rendering: the game's camera is final for this tick.
        void OnBeforeRender()
        {
            try
            {
                var mc = MainCam();
                if (mc != null)
                {
                    var tp = mc.transform.position; var tr = mc.transform.rotation;
                    if (tp != lastCamPos || tr != lastCamRot) camMoves++;
                    lastCamPos = tp; lastCamRot = tr;
                    if (plT == null) { var cm = CameraManager.Instance; if (cm != null && cm._Owner != null) plT = cm._Owner.transform; }
                    if (plT != null) { var pp = plT.position; if (pp != lastPl) plMoves++; lastPl = pp; }
                }
                if (eyesOn == 0 || eyeL == null) return;
                if (snapAtFrame >= 0 && Time.frameCount >= snapAtFrame && eyeFrame >= 0) { Snap(); snapAtFrame = -1; }
                var m = MainCam(); if (m == null) return;
                var t = m.transform; var right = t.right;
                float h = ctrl ? 0f : sep * 0.5f;
                foreach (var (c, s) in new[] { (eyeL, -1f), (eyeR, 1f) })
                {
                    c.transform.SetPositionAndRotation(t.position + right * (s * h), t.rotation);
                    c.fieldOfView = m.fieldOfView; c.nearClipPlane = m.nearClipPlane; c.farClipPlane = m.farClipPlane;
                    c.cullingMask = m.cullingMask;
                }
                eyeFrame = Time.frameCount;
            }
            catch (Exception e) { L.LogError($"beforeRender {e}"); eyesOn = 0; }
        }

        // Called in frame N+1 before rendering: both eye textures still hold frame N's images.
        void Snap()
        {
            string dir = Path.Combine(@"C:\VRLab\evidence\AILimitVR", DateTime.Now.ToString("yyyy-MM-dd"), "eyes");
            Directory.CreateDirectory(dir);
            int f = eyeFrame;
            foreach (var (rt, tag) in new[] { (rtL, "L"), (rtR, "R") })
            {
                var prev = RenderTexture.active; RenderTexture.active = rt;
                var tex = new Texture2D(rt.width, rt.height, TextureFormat.RGBA32, false);
                tex.ReadPixels(new Rect(0, 0, rt.width, rt.height), 0, 0); tex.Apply();
                RenderTexture.active = prev;
                var png = ImageConversion.EncodeToPNG(tex);
                File.WriteAllBytes(Path.Combine(dir, $"{snapName}_f{f}_{tag}.png"), png);
                UnityEngine.Object.Destroy(tex);
            }
            L.LogInfo($"SNAP {snapName} eyes rendered in frame {f}, read in frame {Time.frameCount}, ctrl {ctrl} sep {sep}, cam pos {main.transform.position} dir {dir}");
        }
    }
}

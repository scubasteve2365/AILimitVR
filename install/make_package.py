"""Builds the install folder and zip in ..\\..\\build\\package from a finished build. Run with Python 3.

    make_package.py <version>      for example: make_package.py 1.0

The zip stays local until the author says the build will be shared.
Licence files come from the dependency folders: set AILVR_DEPS to the folder holding openxr_loader_1.1.65, minhook,
imgui\imgui-1.93.0.
"""
import hashlib
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.normpath(os.path.join(HERE, ".."))
BUILD = os.path.normpath(os.path.join(HERE, "..", "..", "build"))
NAME = "AILimitVR"
version = sys.argv[1]
PKG = os.path.join(BUILD, "package", NAME)

if os.path.isdir(PKG):
    shutil.rmtree(PKG)
files = os.path.join(PKG, "files")
mod = os.path.join(files, "AILimitVR")
os.makedirs(mod)
shutil.copy2(os.path.join(BUILD, "winhttp.dll"), files)
for n in ("AILimitVRNative.dll", "openxr_loader.dll"):
    shutil.copy2(os.path.join(BUILD, n), mod)

for name in ("Install AILimitVR.bat", "README.txt", "THIRD-PARTY-NOTICES.txt"):
    with open(os.path.join(HERE, name), "rb") as src:
        text = src.read().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")  # Windows line endings, whatever git did
    with open(os.path.join(PKG, name), "wb") as dst:
        dst.write(text)
DEPS = os.environ.get("AILVR_DEPS", "")
lic = os.path.join(PKG, "licences")
os.makedirs(lic)
shutil.copy2(os.path.join(MOD, "LICENSE"), os.path.join(lic, "AILimitVR-LICENSE.txt"))
shutil.copy2(os.path.join(DEPS, "openxr_loader_1.1.65", "share", "doc", "openxr", "LICENSE"), os.path.join(lic, "OpenXR-loader-LICENSE.txt"))
shutil.copy2(os.path.join(DEPS, "minhook", "LICENSE.txt"), os.path.join(lic, "MinHook-LICENSE.txt"))
shutil.copy2(os.path.join(DEPS, "imgui", "imgui-1.93.0", "LICENSE.txt"), os.path.join(lic, "DearImGui-LICENSE.txt"))

zip_path = os.path.join(BUILD, "package", "%s-%s.zip" % (NAME, version))
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, names in os.walk(PKG):
        for n in sorted(names):
            full = os.path.join(root, n)
            z.write(full, os.path.join(NAME, os.path.relpath(full, PKG)))
h = hashlib.sha256(open(zip_path, "rb").read()).hexdigest()
print(zip_path, os.path.getsize(zip_path) // 1024, "KB", "sha256", h)
for root, _, names in os.walk(PKG):
    for n in sorted(names):
        print("  ", os.path.relpath(os.path.join(root, n), PKG))

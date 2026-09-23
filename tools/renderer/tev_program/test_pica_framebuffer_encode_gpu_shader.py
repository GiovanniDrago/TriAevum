"""The embedded raw-copy encode shader has to compile.

Its GLSL lives inside a C++ header, so a syntax error would otherwise stay
invisible until a machine with shaderc and a device reaches a raw copy - which
is exactly how a reserved word (`sample`) went unnoticed until this check
existed. glslangValidator from the Vulkan SDK catches that class of defect.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
HEADER = (
    REPO
    / "runtime"
    / "three_ds_recomp"
    / "include"
    / "fast"
    / "renderer3ds"
    / "pica_framebuffer_encode_gpu.h"
)
GLSL_PATTERN = re.compile(r'R"glsl\((.*?)\)glsl"', re.S)


def find_glslang_validator() -> str | None:
    on_path = shutil.which("glslangValidator")
    if on_path is not None:
        return on_path
    roots = []
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        roots.append(Path(sdk))
    installations = Path("C:/VulkanSDK")
    if installations.is_dir():
        roots.extend(sorted(installations.iterdir(), reverse=True))
    for root in roots:
        candidate = root / "Bin" / "glslangValidator.exe"
        if candidate.is_file():
            return str(candidate)
    return None


class PicaFramebufferEncodeGpuShaderTests(unittest.TestCase):
    def test_embedded_compute_shader_compiles(self) -> None:
        glslang = find_glslang_validator()
        if glslang is None:
            self.skipTest("glslangValidator is not available")
        match = GLSL_PATTERN.search(HEADER.read_text(encoding="utf-8"))
        self.assertIsNotNone(match, "encode shader source missing from the header")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "pica_framebuffer_encode.comp"
            source.write_text(match.group(1), encoding="utf-8", newline="\n")
            result = subprocess.run(
                [glslang, "-V", str(source)],
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

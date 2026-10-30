"""Negative checks for the fixed-image patcher, run without board access."""

import json
from pathlib import Path
import tempfile
import unittest

import build_diagnostics as build


class ImageGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.metadata = json.loads((build.SOURCES / "baseline.json").read_text(encoding="utf-8"))
        cls.test_root = (build.REPO / "out" / "tests").resolve()
        cls.test_root.mkdir(parents=True, exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(dir=cls.test_root)
        cls.baseline = build.read_bundle(cls.metadata, Path(cls.temporary.name))
        cls.base = (cls.baseline / cls.metadata["base_image"]).read_bytes()
        cls.layout = cls.metadata["layout"]

    @classmethod
    def tearDownClass(cls):
        Path(cls.temporary.name).resolve().relative_to(cls.test_root)
        cls.temporary.cleanup()

    def test_historical_images_are_exactly_recoverable(self):
        offset = self.layout["entry_address"] - self.layout["runtime_base"]
        for item in self.metadata["variants"].values():
            with self.subTest(image=item["image"]):
                image = (self.baseline / item["image"]).read_bytes()
                payload = image[offset:offset + item["payload_size"]]
                self.assertEqual(build.patch_image(self.base, payload, self.layout), image)

    def test_reject_modified_base_or_footer(self):
        for position in [0, len(self.base) - 1]:
            with self.subTest(position=position):
                altered = bytearray(self.base)
                altered[position] ^= 1
                with self.assertRaises(ValueError):
                    build.patch_image(bytes(altered), b"\x00\xbf", self.layout)

    def test_reject_empty_odd_or_oversized_payload(self):
        for payload in [b"", b"x", b"x" * (self.layout["capacity"] + 2)]:
            with self.subTest(size=len(payload)):
                with self.assertRaises(ValueError):
                    build.patch_image(self.base, payload, self.layout)

    def test_reject_region_crossing_image_boundary(self):
        layout = dict(self.layout)
        layout["entry_address"] = layout["runtime_base"] + len(self.base) - 2
        with self.assertRaises(ValueError):
            build.patch_image(self.base, b"\x00\xbf", layout)


if __name__ == "__main__":
    unittest.main()

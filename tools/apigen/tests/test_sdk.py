import unittest

from mirage_apigen import DEFAULT_INPUT
from mirage_apigen.sdk import SDKClass, load, parse


class SDKTests(unittest.TestCase):
    def test_current_dump(self):
        index = load(DEFAULT_INPUT)
        device = index.types["ALAudio.ALAudioDevice"]
        self.assertIsInstance(device, SDKClass)
        prop = next(p for p in device.properties if p.name == "MinOggVorbisDurationGame")
        self.assertEqual((prop.type.kind, prop.offset, prop.flags), ("Float", 480, 16384))
        self.assertTrue(index.functions)
        self.assertFalse([d for d in index.diagnostics if d.severity == "error"])

    def test_relationships_and_nested_arrays(self):
        index = parse([
            {"path": "Core.Object", "kind": "Class"},
            {"path": "P.C", "kind": "Class", "super": "Core.Object", "properties": [
                {"name": "Nested", "kind": "Array", "inner": {"kind": "Array", "inner": "Int"}, "dim": 2},
                {"name": "Ref", "kind": "Object", "type": "P.Missing"},
            ]},
        ])
        self.assertEqual(index.parents, {"P.C": "Core.Object"})
        self.assertEqual(index.dependencies["P.C"], {"Core.Object", "P.Missing"})
        self.assertEqual(index.dependents["Core.Object"], {"P.C"})
        ref = index.types["P.C"].properties[0].type
        self.assertEqual((ref.dimension, ref.inner.inner.kind), (2, "Int"))
        self.assertEqual({d.code for d in index.diagnostics}, {"missing-reference", "missing-functions"})

    def test_bad_metadata(self):
        index = parse([
            {"path": "P.A", "kind": "Class", "super": "P.B", "properties": [
                {"name": "a", "kind": "Int"}, {"name": "A", "kind": "Int"},
                {"name": "bad\nend", "kind": "Str"},
            ]},
            {"path": "P.B", "kind": "Class", "super": "P.A"},
            {"path": "p.a", "kind": "Class"},
            {"path": "../escape", "kind": "Class"},
        ])
        self.assertEqual(index.parents, {})
        self.assertEqual({d.code for d in index.diagnostics}, {
            "duplicate-member", "invalid-property", "duplicate-type", "invalid-record",
            "inheritance-cycle", "missing-functions",
        })

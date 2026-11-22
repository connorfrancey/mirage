import unittest
from pathlib import Path

from mirage_apigen.analysis import TypeResolver, analyze
from mirage_apigen.config import APIExposurePolicy, MemberRule
from mirage_apigen.render import inspection, luals, public_functions
from mirage_apigen.sdk import SDKTypeReference, parse

OWNER = "ALAudio.ALAudioDevice"
GAME = f"{OWNER}.MinOggVorbisDurationGame"
EDITOR = f"{OWNER}.MinOggVorbisDurationEditor"
RECORDS = [{"path": OWNER, "kind": "Class", "properties": [
    {"name": "MinOggVorbisDurationGame", "kind": "Float"},
    {"name": "MinOggVorbisDurationEditor", "kind": "Float"},
    {"name": "Nested", "kind": "Array", "inner": {"kind": "Array", "inner": "Int"}},
]}]


class LuaLSTests(unittest.TestCase):
    def test_primitives(self):
        resolver = TypeResolver(parse([]), APIExposurePolicy())
        for kind, expected in [("Bool", "boolean"), ("Byte", "integer"), ("Int64", "integer"),
                               ("Double", "number"), ("Name", "string"), ("Str", "string")]:
            self.assertEqual(resolver.resolve(SDKTypeReference(kind)), (expected, ""))

    def test_golden(self):
        index = parse(RECORDS)
        policy = APIExposurePolicy(members={GAME: MemberRule(True, "vorbis_seconds", ("vorbis",), deprecated=True)})
        resolver = TypeResolver(index, policy)
        api = analyze(index, resolver)
        actual = luals(list(public_functions(api)))
        self.assertEqual(actual, (Path(__file__).parent / "golden/runtime.lua").read_text())
        editor = inspection(api, resolver)["inspection/package-ALAudio.lua"]
        self.assertIn("---@field Nested integer[][]", editor)
        self.assertNotIn("mirage.sdk", editor)

    def test_deny_and_unsafe_requests(self):
        index = parse(RECORDS)
        policy = APIExposurePolicy(members={GAME: MemberRule(True, writable=True)}, package_deny=("ALAudio",))
        self.assertFalse(analyze(index, TypeResolver(index, policy)).members[GAME]["exposed"])
        policy = APIExposurePolicy(members={GAME: MemberRule(True, writable=True)})
        api = analyze(index, TypeResolver(index, policy))
        self.assertFalse(api.members[GAME]["generated"])
        self.assertIn("write support", api.members[GAME]["reason"])
        self.assertFalse(api.members[EDITOR]["exposed"])

    def test_name_collision(self):
        index = parse(RECORDS)
        policy = APIExposurePolicy(members={GAME: MemberRule(True, "same"), EDITOR: MemberRule(True, "same")})
        api = analyze(index, TypeResolver(index, policy))
        self.assertIn("duplicate-lua-name", {d.code for d in api.diagnostics})

    def test_optional_sdk_function_parameters_are_editor_only(self):
        records = [{**RECORDS[0], "functions": [{
            "name": "Query", "verified": True, "invocation": "process_event", "parameters": [
                {"name": "value", "kind": "Float", "optional": True},
                {"name": "ok", "kind": "Bool", "direction": "out"},
            ], "return": {"name": "result", "kind": "Int"},
        }]}]
        path = f"{OWNER}.Query"
        index = parse(records)
        resolver = TypeResolver(index, APIExposurePolicy(members={path: MemberRule(True)}))
        api = analyze(index, resolver)
        editor = inspection(api, resolver)["inspection/package-ALAudio.lua"]
        self.assertIn("---@overload fun(self: MirageSDK.ALAudio.ALAudioDevice): integer, boolean", editor)
        self.assertIn("---@return boolean ok", editor)
        self.assertFalse(api.members[path]["generated"])

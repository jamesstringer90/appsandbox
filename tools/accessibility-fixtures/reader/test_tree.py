import copy
import unittest

from tree import compare, validate_spec


def snapshot():
    return {
        "schema": "asb-ax-tree-1", "readOnly": True, "trusted": True,
        "complete": True, "issues": [], "pid": 10, "executable": "/fixture",
        "processStartedAt": "start", "roots": [0], "nodes": [
            {"id": 0, "children": [1], "childrenError": 0, "attributes": {
                "AXRole": {"value": "AXWindow"}, "AXTitle": {"value": "Fixture"}}},
            {"id": 1, "children": [], "childrenError": -25205, "attributes": {
                "AXRole": {"value": "AXCheckBox"}, "AXTitle": {"value": "Meal"},
                "AXValue": {"value": 0}, "AXEnabled": {"value": True}},
                "actions": {"value": ["AXPress"]}}
        ]
    }


SPEC = {"version": 1, "cases": [{"name": "checkbox", "match": {"AXTitle": "Meal"},
    "attributes": ["AXRole", "AXValue", "AXEnabled", "@actions"], "expected": {"AXValue": 0}}]}


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.native = snapshot()
        self.public = snapshot()

    def result(self, spec=SPEC, after=None):
        return compare(spec, self.native, self.public, after)

    def test_exact_parity_and_boolean_type(self):
        self.assertEqual(self.result()["status"], "PASS")
        self.public["nodes"][1]["attributes"]["AXEnabled"] = {"value": 1}
        result = self.result()
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["cases"][0]["differences"][0]["path"], "/0/attributes/AXEnabled/value")

    def test_value_and_action_losses(self):
        for field, value in (("AXValue", {"value": 1}), ("@actions", {"value": []})):
            with self.subTest(field=field):
                self.public = snapshot()
                if field == "@actions":
                    self.public["nodes"][1]["actions"] = value
                else:
                    self.public["nodes"][1]["attributes"][field] = value
                self.assertEqual(self.result()["status"], "FAIL")

    def test_unread_observations_never_pass_even_when_equal(self):
        for value in ({"error": -25205}, {"error": -25204}, {"redacted": True},
                      {"unread": True}, {"unsupportedType": 90}, {"externalReference": True}):
            with self.subTest(value=value):
                self.native["nodes"][1]["attributes"]["AXValue"] = value
                self.public["nodes"][1]["attributes"]["AXValue"] = value
                self.assertEqual(self.result()["status"], "INCOMPLETE")

    def test_missing_ambiguous_and_truncated(self):
        self.public["nodes"][1]["attributes"]["AXTitle"] = {"value": "Other"}
        self.assertEqual(self.result()["status"], "INCOMPLETE")
        self.public = snapshot()
        self.public["nodes"][0]["attributes"]["AXTitle"] = {"value": "Meal"}
        self.assertEqual(self.result()["status"], "INCOMPLETE")
        self.public = snapshot()
        self.public["complete"] = False
        self.assertEqual(self.result()["status"], "INCOMPLETE")

    def test_empty_and_broken_graph(self):
        for nodes in ([], [snapshot()["nodes"][0]], snapshot()["nodes"] * 2):
            self.public["nodes"] = nodes
            self.assertEqual(self.result()["status"], "INCOMPLETE")

    def test_baseline_change_precondition_and_process_restart(self):
        after = snapshot()
        after["nodes"][1]["attributes"]["AXValue"] = {"value": 1}
        self.assertEqual(self.result(after=after)["status"], "INCOMPLETE")
        after = snapshot()
        after["processStartedAt"] = "restarted"
        self.assertEqual(self.result(after=after)["status"], "INCOMPLETE")
        self.native["nodes"][1]["attributes"]["AXValue"] = {"value": 1}
        self.public = copy.deepcopy(self.native)
        self.assertEqual(self.result()["status"], "INCOMPLETE")

    def test_subtree_order_aliases_and_local_id_remapping(self):
        spec = {"version": 1, "cases": [{"name": "tree", "match": {"AXRole": "AXWindow"},
            "subtree": True, "attributes": ["AXRole", "AXTitle", "AXValue"]}]}
        for tree in (self.native, self.public):
            tree["nodes"][0]["attributes"]["AXValue"] = {"error": -25205}
            tree["nodes"][1]["children"] = [0]
        self.public["nodes"][0]["id"] = 50
        self.public["nodes"][1]["children"] = [50]
        self.public["roots"] = [50]
        self.assertEqual(self.result(spec)["status"], "PASS")
        self.public["nodes"][1]["children"] = []
        self.assertEqual(self.result(spec)["status"], "FAIL")
        self.public["nodes"][0]["children"] = [1, 50]
        self.assertEqual(self.result(spec)["status"], "FAIL")

    def test_references_and_order_are_compared(self):
        spec = {"version": 1, "cases": [{"name": "tree", "match": {"AXRole": "AXWindow"},
            "subtree": True, "attributes": ["AXRole", "AXTitle", "AXSelectedChildren"]}]}
        for tree in (self.native, self.public):
            tree["nodes"][0]["attributes"]["AXSelectedChildren"] = {"items": [{"ref": 1}]}
            tree["nodes"][1]["attributes"]["AXSelectedChildren"] = {"error": -25205}
        self.assertEqual(self.result(spec)["status"], "PASS")
        self.public["nodes"][0]["attributes"]["AXSelectedChildren"] = {"items": [{"ref": 0}]}
        self.assertEqual(self.result(spec)["status"], "FAIL")
        for tree in (self.native, self.public):
            tree["nodes"][0]["children"] = [1, 0]
        self.public["nodes"][0]["children"] = [0, 1]
        self.assertEqual(self.result(spec)["status"], "FAIL")

    def test_spec_rejects_empty_checks_and_typos(self):
        for spec in ({"version": 1, "cases": []}, {"version": 1, "cases": [
                dict(SPEC["cases"][0], attribute=["AXValue"])]}):
            with self.assertRaises(ValueError):
                validate_spec(spec)


if __name__ == "__main__":
    unittest.main()

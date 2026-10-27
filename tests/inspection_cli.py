"""Inspection protocol mapping and bounded, service-free review reads."""
import contextlib
import copy
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("preview_inspection_cli", str(ROOT / "bin/asset-preview"))
spec = importlib.util.spec_from_loader(loader.name, loader)
client = importlib.util.module_from_spec(spec)
loader.exec_module(client)


def invoke(*args):
    output = io.StringIO()
    with patch.object(sys, "argv", ["asset-preview", *args]), contextlib.redirect_stdout(output):
        result = client.main()
    return result, output.getvalue()


def review():
    return {"schema": "asset-preview-review/v1", "document_revision": 3,
            "asset": {"id": "project/ship", "variant": "active", "lod": 0,
                      "model": "exports/ship.glb", "model_sha256": "a" * 64,
                      "frame": {"up": "+Y", "front": "-Z", "units": "metres"}},
            "annotations": [{"id": "note-00000000-0000-4000-8000-000000000001",
                             "author": {"kind": "human", "name": "local user"},
                             "created_at": "2026-10-16T08:00:00.000Z", "updated_at": "2026-10-16T08:00:00.000Z",
                             "status": "open", "text": "Check the clamp fit.",
                             "tags": ["attachment"], "created_against": "a" * 64,
                             "targets": [{"id": "scene:0/nodes:0/1", "name": "Clamp",
                                          "semantic_id": "clamp", "node_index": 1,
                                          "anchor": {"kind": "surface_point", "position_local": [0, 1, 0],
                                                     "normal_local": [0, 1, 0]}}],
                             "view": {"camera": {"position": [3, 2, 3], "focal": [0, 0, 0],
                                                  "up": [0, 1, 0], "angle": 30, "parallel_span": 5},
                                      "orthographic": False, "cuts": [], "hidden": []}}]}


class InspectionCli(unittest.TestCase):
    def test_live_commands_map_without_starting_or_raising(self):
        cases = [
            (("open", "ship", "--review", "source/inspection/ship.json", "--asset-id", "project/ship", "--variant", "active"),
             {"review": str(Path("source/inspection/ship.json").absolute()), "asset_id": "project/ship", "variant": "active"}),
            (("close", "ship"), {}), (("state", "ship"), {}),
            (("parts", "ship", "--offset", "12", "--limit", "17"), {"offset": 12, "limit": 17}),
            (("select", "ship", "--target", "scene:0/nodes:0/1"), {"target": "scene:0/nodes:0/1"}),
            (("hide", "ship", "--target", "scene:0/nodes:0/1"), {"target": "scene:0/nodes:0/1"}),
            (("show", "ship", "--target", "scene:0/nodes:0/1"), {"target": "scene:0/nodes:0/1"}),
            (("isolate", "ship", "--target", "scene:0/nodes:0/1"), {"target": "scene:0/nodes:0/1"}),
            (("restore", "ship", "--all"), {"all": True}),
            (("restore", "ship", "--last"), {"last": True}),
            (("cut", "ship", "--axis", "z", "--position", "-0.5", "--keep", "negative", "--no-enabled"),
             {"axis": "z", "position": -0.5, "keep": "negative", "enabled": False}),
        ]
        with patch.object(client, "start", side_effect=AssertionError("unexpected start")), \
             patch.object(client, "running", side_effect=AssertionError("unexpected lifecycle probe")), \
             patch.object(client, "rpc", return_value={"ok": True}) as rpc:
            for arguments, fields in cases:
                with self.subTest(arguments=arguments):
                    _, output = invoke("inspect", *arguments, "--json")
                    self.assertEqual(json.loads(output), {"ok": True})
                    self.assertEqual(rpc.call_args.args[0], {"method": "inspect", "id": "ship", "action": arguments[0], **fields})
            self.assertEqual(rpc.call_count, len(cases))

    def test_parser_rejects_ambiguous_and_unbounded_requests(self):
        cases = [("inspect", "parts", "ship", "--limit", "101"),
                 ("inspect", "parts", "ship", "--offset", "-1"),
                 ("inspect", "restore", "ship"),
                 ("inspect", "restore", "ship", "--all", "--last"),
                 ("inspect", "select", "ship"),
                 ("inspect", "cut", "ship", "--axis", "z", "--position", "NaN"),
                 ("inspect", "cut", "ship", "--axis", "z", "--position", "1e999"),
                 ("annotations", "remove", "ship", "--note", "note-one"),
                 ("annotations", "update", "ship", "--note", "note-one", "--text", "Fit", "--expected-revision", "0")]
        with patch.object(client, "rpc") as rpc, contextlib.redirect_stderr(io.StringIO()):
            for arguments in cases:
                with self.subTest(arguments=arguments), self.assertRaises(SystemExit):
                    invoke(*arguments)
            rpc.assert_not_called()

    def test_mutations_use_current_revision_and_digest_for_compare_and_swap(self):
        state = {"ok": True, "document_revision": 3, "review_digest": "b" * 64}
        saved = {"ok": True, "document_revision": 4}
        with patch.object(client, "start", side_effect=AssertionError("unexpected start")), \
             patch.object(client, "rpc", side_effect=[state, saved]) as rpc:
            invoke("annotations", "add", "ship", "--text", "Check clamp", "--tag", "fit", "--json")
            self.assertEqual(rpc.call_args_list[0].args[0], {"method": "inspect", "id": "ship", "action": "state"})
            self.assertEqual(rpc.call_args_list[1].args[0],
                             {"method": "annotations", "id": "ship", "action": "add",
                              "note": {"text": "Check clamp", "status": "open", "tags": ["fit"]},
                              "expected_revision": 3, "expected_digest": "b" * 64})
        with patch.object(client, "rpc", return_value=saved) as rpc:
            invoke("annotations", "update", "ship", "--note", "note-one", "--status", "resolved",
                   "--clear-tags", "--expected-revision", "3", "--expected-digest", "b" * 64)
            self.assertEqual(rpc.call_args.args[0], {"method": "annotations", "id": "ship", "action": "update",
                                                   "note_id": "note-one", "note": {"status": "resolved", "tags": []},
                                                   "expected_revision": 3, "expected_digest": "b" * 64})
            invoke("annotations", "remove", "ship", "--note", "note-one", "--expected-revision", "4")
            self.assertEqual(rpc.call_args.args[0], {"method": "annotations", "id": "ship", "action": "remove",
                                                   "note_id": "note-one", "expected_revision": 4})

    def test_invalid_mutation_never_fetches_or_sends(self):
        with patch.object(client, "rpc") as rpc:
            for arguments in [("update", "ship", "--note", "note-one"),
                              ("add", "ship", "--text", " "),
                              ("add", "ship", "--text", "x" * 8193)]:
                with self.subTest(arguments=arguments[:1]), self.assertRaises(ValueError):
                    invoke("annotations", *arguments)
            rpc.assert_not_called()

    def test_first_annotation_compares_against_missing_document(self):
        with patch.object(client, "rpc", side_effect=[{"ok": True, "document_revision": 0, "review_digest": ""},
                                                     {"ok": True, "document_revision": 1}]) as rpc:
            invoke("annotations", "add", "ship", "--text", "First note", "--json")
            request = rpc.call_args.args[0]
            self.assertEqual(request["expected_revision"], 0)
            self.assertEqual(request["expected_digest"], "")

    def test_offline_list_and_export_need_no_service(self):
        with tempfile.TemporaryDirectory(prefix="preview-review-cli-") as temporary:
            source = Path(temporary) / "ship.json"
            source.write_text(json.dumps(review()))
            destination = Path(temporary) / "report.md"
            with patch.object(client, "start", side_effect=AssertionError("unexpected start")), \
                 patch.object(client, "running", side_effect=AssertionError("unexpected lifecycle probe")), \
                 patch.object(client, "rpc", side_effect=AssertionError("unexpected RPC")):
                _, output = invoke("annotations", "list", str(source), "--status", "open", "--json")
                result = json.loads(output)
                self.assertEqual(result["total"], 1)
                self.assertEqual(result["annotations"][0]["targets"][0]["id"], "scene:0/nodes:0/1")
                invoke("annotations", "export", str(source), "--format", "markdown", "--output", str(destination))
                self.assertIn("Saved view:", destination.read_text())
                self.assertIn("scene:0/nodes:0/1", destination.read_text())
                with self.assertRaises(FileExistsError):
                    invoke("annotations", "export", str(source), "--output", str(destination))
                self.assertEqual(json.loads(source.read_text()), review())

    def test_offline_pages_obey_byte_budget_and_filter_before_offset(self):
        document = review()
        template = document["annotations"][0]
        document["annotations"] = []
        for index in range(100):
            note = copy.deepcopy(template)
            note.update(id=f"note-00000000-0000-4000-8000-{index + 1:012x}",
                        status="open" if index % 2 == 0 else "resolved", metadata="x" * 12000)
            document["annotations"].append(note)
        client.validate_review(document)
        result = client.annotation_page(document, 1, 100, "open")
        self.assertEqual(result["total"], 50)
        self.assertEqual(result["annotations"][0]["id"], "note-00000000-0000-4000-8000-000000000003")
        self.assertTrue(result["has_more"])
        self.assertLess(len(result["annotations"]), 49)
        self.assertLessEqual(len(json.dumps(result, ensure_ascii=False, indent=2).encode()) + 1, client.REVIEW_PAGE_BYTES)
        empty = client.annotation_page(document, 1000, 100)
        self.assertEqual(empty["annotations"], [])
        self.assertFalse(empty["has_more"])

    def test_offline_input_rejects_symlinks_and_nonregular_files(self):
        with tempfile.TemporaryDirectory(prefix="preview-review-files-") as temporary:
            source = Path(temporary) / "ship.json"
            source.write_text(json.dumps(review()))
            link = Path(temporary) / "link.json"
            link.symlink_to(source)
            fifo = Path(temporary) / "review.fifo"
            os.mkfifo(fifo)
            with self.assertRaises(OSError):
                client.read_review(str(link))
            with self.assertRaisesRegex(ValueError, "regular file"):
                client.read_review(str(fifo))

    def test_offline_source_labels_preserve_multiline_data(self):
        document = review()
        target = document["annotations"][0]["targets"][0]
        controls = "".join(chr(value) for value in range(1, 32)) + "\x7f"
        for field in ("name", "exported_name", "semantic_id"):
            target[field] = "Clamp" + controls + "<panel>"
        target["id"] = "scene:\t0/nodes:0/1"
        document["asset"]["id"] = "project/\tship"
        document["asset"]["model"] = "exports/ship\tlarge.glb"
        document["annotations"][0]["author"]["name"] = "User\nName"
        document["annotations"][0]["tags"] = ["fit\tcheck"]
        document["annotations"][0]["text"] = "Check\x1b[2Jthe\tclamp\nNext line."
        document["extension"] = {"key\x01": controls}
        with tempfile.TemporaryDirectory(prefix="preview-review-labels-") as temporary:
            source = Path(temporary) / "ship.json"
            source.write_text(json.dumps(document))
            with patch.object(client, "rpc", side_effect=AssertionError("unexpected RPC")):
                _, output = invoke("annotations", "list", str(source), "--json")
                self.assertEqual(json.loads(output)["annotations"][0]["targets"][0], target)
                _, report = invoke("annotations", "export", str(source))
                self.assertIn("Clamp", report)
                self.assertIn("&lt;panel&gt;", report)
                self.assertNotIn("\x1b", report)
                _, text = invoke("annotations", "list", str(source))
                self.assertNotIn("\x1b", text)
                self.assertIn("Check\\u001b[2Jthe\\u0009clamp\nNext line.", text)
        for field in ("name", "exported_name", "semantic_id", "id"):
            broken = copy.deepcopy(document)
            broken["annotations"][0]["targets"][0][field] += "\0"
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "control character"):
                client.validate_review(broken)

    def test_live_command_arguments_keep_strict_control_validation(self):
        with patch.object(client, "rpc") as rpc:
            for arguments in [("annotations", "add", "ship", "--text", "Check\x1b[2Jclamp"),
                              ("inspect", "select", "ship", "--target", "scene:\t0")]:
                with self.subTest(arguments=arguments), self.assertRaisesRegex(ValueError, "control character"):
                    invoke(*arguments)
            rpc.assert_not_called()

    def test_markdown_preserves_non_linefeed_controls_as_visible_text(self):
        document = review()
        document["annotations"][0]["text"] = "One\vTwo\fThree\x1cFour\nLast"
        report = client.review_markdown(client.validate_review(document))
        self.assertIn("> One\\\\u000bTwo\\\\u000cThree\\\\u001cFour\n> Last", report)
        for char in "\v\f\x1c":
            self.assertNotIn(char, report)

    def test_optional_creation_dependency_requires_a_digest(self):
        for value in (None, "", "a" * 63, "g" * 64, 123, True):
            document = review()
            document["annotations"][0]["dependency_revision"] = value
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "dependency_revision"):
                client.validate_review(document)
        document = review()
        document["annotations"][0]["dependency_revision"] = "b" * 64
        self.assertEqual(client.validate_review(document), document)
        del document["annotations"][0]["dependency_revision"]
        self.assertEqual(client.validate_review(document), document)

    def test_review_schema_and_limits_are_strict(self):
        for mutate in [lambda doc: doc.update(schema="other/v1"),
                       lambda doc: doc.update(document_revision=True),
                       lambda doc: doc.update(document_revision=0),
                       lambda doc: doc.update(asset=[]),
                       lambda doc: doc.update(annotations={}),
                       lambda doc: doc["annotations"].append(copy.deepcopy(doc["annotations"][0])),
                       lambda doc: doc["annotations"][0].update(status="done"),
                       lambda doc: doc["annotations"][0].update(id="invalid-id"),
                       lambda doc: doc["annotations"][0].update(updated_at="2025-10-16T08:00:00Z"),
                       lambda doc: doc["annotations"][0]["view"].update(cuts=[{"axis": "z", "position": 0, "enabled": "yes"}]),
                       lambda doc: doc["annotations"][0].update(text="\U0001f680" * 2049),
                       lambda doc: doc["annotations"][0]["targets"][0]["anchor"].update(position_local=[0, 1]),
                       lambda doc: doc["annotations"][0]["view"].update(camera={"up": [0, float("inf"), 0]})]:
            document = review()
            mutate(document)
            with self.assertRaises(ValueError):
                client.validate_review(document)
        document = review()
        document["annotations"] *= 4097
        with self.assertRaises(ValueError):
            client.validate_review(document)
        with tempfile.TemporaryDirectory(prefix="preview-review-limit-") as temporary:
            filename = Path(temporary) / "large.json"
            filename.write_bytes(b" " * (client.REVIEW_FILE_LIMIT + 1))
            with self.assertRaisesRegex(ValueError, "16 MiB"):
                client.read_review(str(filename))
        nested = {}
        for _ in range(34):
            nested = {"nested": nested}
        document = review()
        document["extension"] = nested
        with self.assertRaisesRegex(ValueError, "nesting"):
            client.validate_review(document)

    def test_json_rejects_nonfinite_and_duplicate_fields(self):
        for source in ['{"x":NaN}', '{"x":Infinity}', '{"x":-Infinity}', '{"x":1e999}', '{"x":1,"x":2}']:
            with self.subTest(source=source), self.assertRaises(ValueError):
                client.json_value(source)
        with patch.object(client.socket, "socket") as socket:
            with self.assertRaises(ValueError):
                client.rpc({"method": "inspect", "position": float("nan")})
            socket.assert_not_called()
        with patch.object(client, "start"), patch.object(client, "rpc") as rpc:
            with self.assertRaises(ValueError):
                invoke("add", "/tmp/ship.glb", "--settings", '{"light":NaN}')
            rpc.assert_not_called()

    def test_markdown_escapes_metadata_and_note_markup(self):
        document = review()
        document["asset"]["id"] = '<img src=x onerror="bad">'
        document["annotations"][0]["text"] = '![click](javascript:bad)\n<script>bad</script>\n```'
        report = client.review_markdown(client.validate_review(document))
        self.assertNotIn("<img", report)
        self.assertNotIn("<script>", report)
        self.assertNotIn("![click](javascript:bad)", report)
        self.assertIn("&lt;script&gt;", report)
        self.assertIn("Created against:", report)
        self.assertIn("does not verify current bindings", report)


if __name__ == "__main__":
    unittest.main(verbosity=2)

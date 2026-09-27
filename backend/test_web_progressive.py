"""Run with: python -m unittest backend/test_web_progressive.py"""

import ast
import re
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

BACKEND = Path(__file__).parent
sys.path.insert(0, str(BACKEND))

import web_search  # noqa: E402


def load_sentence_splitter():
    """Load only the pure splitter without importing audio/STT dependencies."""
    source = (BACKEND / "voice.py").read_text(encoding="utf-8")
    tree = ast.parse(source)
    function = next(
        node for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == "split_complete_sentences"
    )
    module = ast.Module(body=[function], type_ignores=[])
    namespace = {"re": re}
    exec(compile(module, str(BACKEND / "voice.py"), "exec"), namespace)
    return namespace["split_complete_sentences"]


class FakeDDGS:
    def __init__(self, timeout):
        self.timeout = timeout

    def text(self, query, max_results):
        return [
            {"title": f"Result {index}", "href": f"https://example.com/{index}", "body": query}
            for index in range(max_results)
        ]


class ProgressiveWebTests(unittest.TestCase):
    def setUp(self):
        web_search._search_cache.clear()

    def test_continuation_phrases(self):
        for phrase in (
            "continue", "Donne-moi plus d'informations", "Dis-m'en plus",
            "Tell me more", "Continue the search",
        ):
            with self.subTest(phrase=phrase):
                self.assertTrue(web_search.is_web_continuation(phrase))
        self.assertFalse(web_search.is_web_continuation("Continue la musique"))

    @patch.object(web_search, "DDGS", FakeDDGS)
    @patch.object(web_search, "fetch_page_text", return_value="page")
    def test_background_search_fetches_two_pages(self, fetch):
        result = web_search.search_web("actualite volvo cars")
        self.assertEqual(len(result["results"]), 5)
        self.assertEqual(fetch.call_count, 2)

    def test_first_spoken_chunk_is_five_words(self):
        split = load_sentence_splitter()
        chunks, remainder = split(
            "An iPhone 13 refurbished online currently costs about 300 euros.",
            max_words=5,
        )
        self.assertEqual(chunks[0], "An iPhone 13 refurbished online")
        self.assertEqual(chunks[1], "currently costs about 300 euros.")
        self.assertEqual(remainder, "")


if __name__ == "__main__":
    unittest.main()

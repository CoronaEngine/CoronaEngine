from __future__ import annotations

import unittest

from editor.plugins.AITool.services.intent_understanding import IntentUnderstandingService


class IntentUnderstandingDiscussionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.service = IntentUnderstandingService()

    def test_topic_discussion_does_not_become_plan_drafting(self) -> None:
        decision = self.service.classify(
            "@小女孩 围绕迪士尼乐园主题讨论一下",
            allow_llm=False,
        )

        self.assertEqual(decision.intent, "discussion")

    def test_explicit_design_request_becomes_plan_drafting(self) -> None:
        decision = self.service.classify(
            "@小女孩 按照迪士尼风格来设计一个卧室",
            allow_llm=False,
        )

        self.assertEqual(decision.intent, "plan_drafting")

    def test_greeting_stays_discussion(self) -> None:
        decision = self.service.classify("@小女孩 你好", allow_llm=False)

        self.assertEqual(decision.intent, "discussion")

    def test_gm_delegation_becomes_plan_drafting(self) -> None:
        decision = self.service.classify(
            "@GM 让@小女孩 给我一个方案",
            allow_llm=False,
        )

        self.assertEqual(decision.intent, "plan_drafting")

    def test_plan_request_beats_status_keyword_matching(self) -> None:
        decision = self.service.classify("@长者 请你给出一个方案", allow_llm=False)

        self.assertEqual(decision.intent, "plan_drafting")

    def test_explicit_progress_question_stays_status_query(self) -> None:
        decision = self.service.classify("@GM 方案进度如何", allow_llm=False)

        self.assertEqual(decision.intent, "status_query")


if __name__ == "__main__":
    unittest.main()

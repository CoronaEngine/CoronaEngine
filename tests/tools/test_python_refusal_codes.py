"""Every code a Python bridge service refuses with is in the contract vocabulary.

Refusals reach Web as Promise rejections now, so the Python-side codes are part of the
same vocabulary as the C++ ones. Data-level codes (FileScripts run records, archive
diagnostics) are not in the scan: they never reject.
"""

import rejection_codes


def test_python_refusal_codes_are_declared():
    undeclared = rejection_codes.python_refusal_codes() - rejection_codes.ALL
    assert not undeclared, (
        f"Python services refuse with codes that are not in rejection_codes.ALL: "
        f"{sorted(undeclared)}; declare them in tests/tools/rejection_codes.py and the contract")


def test_the_document_codes_are_declared():
    for code in ("DOCUMENT_CONFLICT", "INVALID_PROJECT_DOCUMENT", "RUN_ACTIVE", "NO_ACTIVE_PROJECT"):
        assert code in rejection_codes.ALL, code

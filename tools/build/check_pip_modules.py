"""检查 requirements 中列出的 pip 模块是否已安装及版本是否满足。

特性：
1. 解析 requirements.txt（支持基本格式：包名[extra]==x.y.z、>=、>、<=、<、~=、!= 等）
2. 检测是否安装、版本是否满足
3. 可选：自动安装缺失或不满足版本的包 (--auto-install)
4. 可选：输出 JSON (--json)
5. 根据结果设置退出码（全部满足=0，有缺失/不满足=1）

注意：本脚本不执行 hash 校验，不解析 -r 递归文件；如需可扩展。
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import sys
import subprocess
import importlib.util
from typing import Iterable, List, Tuple
from importlib import metadata

# 尝试使用 packaging 解析更复杂的版本约束
try:  # Python 环境可能未装 packaging
    from packaging.requirements import Requirement  # type: ignore
    from packaging.version import Version  # type: ignore
    from packaging.specifiers import SpecifierSet  # type: ignore
except Exception:  # 回退模式（功能有限）
    Requirement = None  # type: ignore
    Version = None  # type: ignore
    SpecifierSet = None  # type: ignore


def get_installed_version(package_name: str) -> str | None:
    """Return installed version string for a package or None if not installed."""
    try:
        return metadata.version(package_name)
    except Exception:
        return None


def version_satisfies(installed: str, spec: str | None) -> bool:
    """Check whether installed version satisfies a given spec string.

    - If `packaging` is available, use `SpecifierSet` for accurate comparison.
    - Fallback: support minimal '==x.y.z' and '>=x.y' comparisons; otherwise return True.
    """
    if not spec:
        return True
    if SpecifierSet is not None and Version is not None:
        try:
            return Version(installed) in SpecifierSet(spec)
        except Exception:
            # Fallback to a simple heuristic if parsing fails
            pass
    # Simple fallback: only '==...' and '>=...'
    m_eq = re.fullmatch(r"==\s*([0-9][0-9a-zA-Z\._-]*)", spec)
    if m_eq:
        return installed == m_eq.group(1)
    m_ge = re.fullmatch(r">=\s*([0-9][0-9a-zA-Z\._-]*)", spec)
    if m_ge:
        return installed.split('.') >= m_ge.group(1).split('.')
    # Not supported operators in fallback path -> assume ok to avoid false negatives
    return True


@dataclasses.dataclass
class RequirementEntry:
    raw_line: str
    name: str
    extras: List[str]
    spec: str | None
    markers: str | None
    line_no: int

    @property
    def display(self) -> str:
        base = self.name
        if self.extras:
            base += f"[{','.join(self.extras)}]"
        if self.spec:
            base += self.spec
        return base


REQ_LINE_RE = re.compile(r"^\s*([A-Za-z0-9_.\-]+)(\[[A-Za-z0-9_,\-]+\])?\s*([!=<>~]{1,2}[^;#\s]+)?(?:\s*;\s*([^#]+))?\s*$")


def parse_requirements(path: str) -> List[RequirementEntry]:
    entries: List[RequirementEntry] = []
    with open(path, 'r', encoding='utf-8') as f:
        for idx, line in enumerate(f, 1):
            raw = line.rstrip('\n')
            stripped = line.strip()
            if not stripped or stripped.startswith('#'):
                continue
            if Requirement is not None:
                try:
                    req = Requirement(stripped)
                    name = req.name
                    extras = list(req.extras)
                    spec = str(req.specifier) if req.specifier else None
                    markers = str(req.marker) if req.marker else None
                    entries.append(RequirementEntry(raw, name, extras, spec, markers, idx))
                    continue
                except Exception:
                    pass  # 回退到正则
            m = REQ_LINE_RE.match(stripped)
            if not m:
                print(f"[WARN] Unable to parse line {idx}: {raw}", file=sys.stderr)
                continue
            name = m.group(1)
            extras_part = m.group(2)
            extras = []
            if extras_part:
                extras = [e.strip() for e in extras_part.strip('[]').split(',') if e.strip()]
            spec = m.group(3)
            markers = m.group(4)
            entries.append(RequirementEntry(raw, name, extras, spec, markers, idx))
    return entries


def check_entry(entry: RequirementEntry) -> Tuple[bool, str, str | None]:
    version = get_installed_version(entry.name)
    if version is None:
        return False, 'missing', None
    if not version_satisfies(version, entry.spec):
        return False, 'version-mismatch', version
    # marker 检测（当前简单跳过，未来可根据 packaging.marker 实现）
    return True, 'ok', version


def auto_install(entry: RequirementEntry) -> int:
    """执行 pip 安装，返回退出码。"""
    spec = entry.name
    if entry.extras:
        spec += f"[{','.join(entry.extras)}]"
    if entry.spec:
        spec += entry.spec
    cmd = [sys.executable, '-m', 'pip', 'install', spec]
    print(f"[AUTO-INSTALL] {' '.join(cmd)}")
    return subprocess.call(cmd)


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Check Python packages against requirements.txt")
    p.add_argument('-r', '--requirements', default=os.path.join(os.path.dirname(__file__), 'requirements.txt'), help='Path to requirements file (default: requirements.txt next to this script)')
    p.add_argument('--json', action='store_true', help='Output result in JSON')
    p.add_argument('--auto-install', action='store_true', help='Auto install missing or mismatched packages')
    p.add_argument('--fail-missing', action='store_true', help='Exit with code 1 when packages are missing (default behavior)')
    p.add_argument('--fail-mismatch', action='store_true', help='Exit with code 1 when version mismatches exist (default behavior)')
    p.add_argument('--ignore-errors', action='store_true', help='Ignore parse errors (default: ignore and warn)')
    p.add_argument('--no-unicode', action='store_true', help='Use ASCII symbols to avoid mojibake on legacy terminals')
    return p


def main(argv: List[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    req_path = args.requirements
    if not os.path.isfile(req_path):
        print(f"[ERROR] Requirements file does not exist: {req_path}", file=sys.stderr)
        return 2
    entries = parse_requirements(req_path)
    results = []
    missing = []
    mismatch = []
    for e in entries:
        ok, status, version = check_entry(e)
        results.append({
            'line': e.line_no,
            'requirement': e.display,
            'name': e.name,
            'spec': e.spec,
            'status': status,
            'installed_version': version,
        })
        if not ok:
            if status == 'missing':
                missing.append(e)
            elif status == 'version-mismatch':
                mismatch.append(e)
            if args.auto_install:
                rc = auto_install(e)
                if rc != 0:
                    print(f"[ERROR] Auto-install failed: {e.display}", file=sys.stderr)
    # 如果启用了自动安装，对失败的重新检查（仅对 missing/mismatch）
    if args.auto_install and (missing or mismatch):
        missing = []
        mismatch = []
        for e in entries:
            ok, status, version = check_entry(e)
            # 更新对应 result (简化: 不更新; 或者可在此修正)
            if not ok:
                if status == 'missing':
                    missing.append(e)
                elif status == 'version-mismatch':
                    mismatch.append(e)

    if args.json:
        payload = {
            'requirements_file': req_path,
            'total': len(entries),
            'ok': len(entries) - (len(missing) + len(mismatch)),
            'missing': [e.display for e in missing],
            'version_mismatch': [e.display for e in mismatch],
            'details': results,
        }
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        # 根据终端编码自动决定是否使用 Unicode 符号；或用户强制关闭
        def _symbols(disable: bool):
            if disable:
                return '+', '-'
            ok_sym, fail_sym = '✓', '✗'
            enc = getattr(sys.stdout, 'encoding', None) or 'utf-8'
            try:
                (ok_sym + fail_sym).encode(enc)
            except Exception:
                return '+', '-'
            # 某些编码声明为 cp936 但仍可能失败，做一次更细测试
            try:
                for ch in (ok_sym, fail_sym):
                    ch.encode(enc)
            except Exception:
                return '+', '-'
            return ok_sym, fail_sym

        ok_mark, fail_mark = _symbols(args.no_unicode)
        print(f"Requirements file: {req_path}")
        for r in results:
            mark = ok_mark if r['status'] == 'ok' else fail_mark
            spec_part = r['spec'] or ''
            ver_part = f" (installed: {r['installed_version']})" if r['installed_version'] else ''
            print(f" {mark} {r['name']}{spec_part}{ver_part} -> {r['status']}")
        if missing:
            print("\nMissing packages:")
            for e in missing:
                print(f"  - {e.display}")
        if mismatch:
            print("\nVersion mismatches:")
            for e in mismatch:
                print(f"  - {e.display}")
    exit_code = 0
    if (missing and (args.fail_missing or not (args.fail_missing is False))) or (mismatch and (args.fail_mismatch or not (args.fail_mismatch is False))):
        exit_code = 1
    return exit_code


if __name__ == '__main__':  # pragma: no cover
    sys.exit(main())
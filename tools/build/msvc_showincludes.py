"""Normalize MSVC dependency lines for Ninja, independent of console code page."""

import locale
import subprocess
import sys


def dependency_prefixes(raw_prefix, ansi_encoding):
    prefixes = []
    for source_encoding in ("utf-8", ansi_encoding):
        try:
            text = raw_prefix.decode(source_encoding)
        except UnicodeError:
            continue
        for output_encoding in ("utf-8", ansi_encoding):
            try:
                entry = (text.encode(output_encoding), output_encoding)
            except UnicodeError:
                continue
            if entry not in prefixes:
                prefixes.append(entry)
    return prefixes


def normalize_line(line, prefixes):
    for prefix, encoding in prefixes:
        if line.startswith(prefix):
            try:
                path = line[len(prefix):].lstrip().decode(encoding)
            except UnicodeDecodeError:
                # An ASCII prefix does not identify the path's code page.
                continue
            return b"Note: including file: " + path.encode("utf-8")
    return line


def main():
    prefixes = dependency_prefixes(bytes.fromhex(sys.argv[1]), locale.getencoding())
    with subprocess.Popen(sys.argv[2:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as compiler:
        for line in compiler.stdout:
            sys.stdout.buffer.write(normalize_line(line, prefixes))
            sys.stdout.buffer.flush()
        return compiler.wait()


if __name__ == "__main__":
    sys.exit(main())

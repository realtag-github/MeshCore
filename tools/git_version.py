Import("env")

import subprocess


def _get_git_hash():
    try:
        return (
            subprocess.check_output(["git", "rev-parse", "--short", "HEAD"])
            .decode("utf-8")
            .strip()
        )
    except Exception:
        return "unknown"


git_hash = _get_git_hash()
# Define as a string literal for the compiler (escape quotes).
env.Append(CPPDEFINES=[("GIT_HASH", '\\"%s\\"' % git_hash)])

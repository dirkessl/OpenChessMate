"""PlatformIO pre-build script for ChessConnect private source handling.

All files in src/chessconnect/ are git-crypt encrypted in the public repo.
When any file is encrypted (or missing), this script excludes the entire folder
from compilation and builds without ChessConnect features.

When all sources are decrypted, they compile normally and CHESSCONNECT_ENABLED is
defined, enabling ChessConnect support in the firmware.

The CI workflow (release.yml) unlocks via git-crypt and builds with full features.
"""

import os
Import("env")

CHESSCONNECT_DIR = os.path.join(env.subst("$PROJECT_SRC_DIR"), "chessconnect")


def is_encrypted_or_missing(filepath):
    """Return True if the file is git-crypt encrypted or missing."""
    try:
        with open(filepath, "rb") as f:
            return f.read(10).startswith(b"\x00GITCRYPT")
    except (FileNotFoundError, IOError):
        return True


# Check every file in the chessconnect folder
all_files = os.listdir(CHESSCONNECT_DIR) if os.path.isdir(CHESSCONNECT_DIR) else []
sources_available = len(all_files) > 0 and not any(
    is_encrypted_or_missing(os.path.join(CHESSCONNECT_DIR, f))
    for f in all_files
)

if sources_available:
    print(">> ChessConnect: sources available - building with CHESSCONNECT_ENABLED")
    env.Append(CPPDEFINES=["CHESSCONNECT_ENABLED"])
else:
    print(">> ChessConnect: sources encrypted/missing - building without CHESSCONNECT_ENABLED")

    def skip_chessconnect(node):
        path = node.get_path().replace("\\", "/")
        if "/chessconnect/" in path:
            return None
        return node

    env.AddBuildMiddleware(skip_chessconnect)

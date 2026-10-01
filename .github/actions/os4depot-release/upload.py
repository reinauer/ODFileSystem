#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Submit an existing archive using OS4Depot's anonymous FTP protocol.

Protocol: https://os4depot.net/index.php?function=ftpinfo
Example: AmigaLabs/clib4/.github/actions/os4depot-release/action.yml
"""

import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    archive = Path(os.environ["OS4DEPOT_ARCHIVE"])
    filename = os.environ["OS4DEPOT_FILENAME"]
    version = os.environ["OS4DEPOT_VERSION"].removeprefix("v")
    passphrase = os.environ["OS4DEPOT_PASSPHRASE"]
    template = Path(os.environ["OS4DEPOT_README"]).read_text(encoding="utf-8")

    if not archive.is_file() or archive.stat().st_size == 0:
        raise ValueError("Release archive is missing or empty")
    if not re.fullmatch(r"[a-z0-9][a-z0-9._-]*\.(lha|zip|tar|gz|bz2|xz)", filename):
        raise ValueError("Use a lowercase archive filename without directories")
    if not re.fullmatch(r"[0-9][A-Za-z0-9.+-]{0,9}", version):
        raise ValueError("OS4Depot version must be 1-10 characters, starting with a digit")
    if not passphrase or len(passphrase) > 40 or any(ord(c) < 32 for c in passphrase):
        raise ValueError("OS4DEPOT_PASSPHRASE must contain 1-40 characters on one line")
    if "\nhend:\n" not in template:
        raise ValueError("OS4Depot readme must contain a hend: header terminator")
    for marker in ("[VERSION]", "[PASSPHRASE]"):
        if template.count(marker) != 1:
            raise ValueError("OS4Depot readme must contain each placeholder exactly once")

    values = {"[VERSION]": version, "[PASSPHRASE]": passphrase}
    readme = re.sub(r"\[VERSION\]|\[PASSPHRASE\]", lambda m: values[m[0]], template)
    stem, extension = filename.rsplit(".", 1)
    readme_name = f"{stem}_{extension}.readme"

    # Keep the secret-bearing readme out of uploaded build artifacts and
    # remove it on both successful and failed transfers.
    with tempfile.TemporaryDirectory(prefix="os4depot-") as directory:
        readme_path = Path(directory) / readme_name
        readme_path.write_text(readme, encoding="utf-8")
        readme_path.chmod(0o600)
        # The readme triggers server processing and must arrive LAST.
        for source, destination in ((archive, filename), (readme_path, readme_name)):
            subprocess.run(
                ["curl", "--fail", "--silent", "--show-error",
                 "--connect-timeout", "30", "--max-time", "300",
                 "--user", "ftp:", "--upload-file", str(source.resolve()),
                 f"ftp://os4depot.net/upload/{destination}"],
                check=True,
            )
    print("Submitted to OS4Depot; acceptance and publication are handled by the site.")


if __name__ == "__main__":
    main()

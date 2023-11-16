#!/usr/bin/env python3
import logging as log
import argparse
import urllib.request
import sys
import shutil
from subprocess import run
from pathlib import Path

# Configure logging
log.basicConfig(
    level=log.DEBUG,
    format="%(asctime)s - %(levelname)s - %(message)s",
    handlers=[log.StreamHandler(sys.stdout)],  # Logging to stdout
)


def download_github_asset(args):
    """Download"""

    src_filename = f"{args.project_name.lower()}-{args.project_version}.tar.gz"
    src_filepath = Path().cwd() / src_filename
    orig_filepath = src_filepath.with_name(f"{args.project_name.lower()}_{args.project_version}.orig.tar.gz")

    if src_filepath.exists():
        log.info(f"'{src_filepath}' exists; skipping download")
        return

    url = "/".join(
        [
            "https://github.com",
            args.org_name,
            args.project_name,
            "releases",
            "download",
            f"v{args.project_version}",
            src_filename,
        ]
    )

    with urllib.request.urlopen(url) as response:
        with src_filepath.open("wb") as content:
            content.write(response.read())

    run(["cp", f"{src_filepath}", ])
    shutil.copy

def parse_args():
    parser = argparse.ArgumentParser(
        description="Create package using release from tarball from GitHUB"
    )

    parser.add_argument(
        "--org-name", default="OpenMPDK", help="The organization name on GitHUB"
    )
    parser.add_argument(
        "--project-name", default="xNVMe", help="The project name on GitHUB"
    )
    parser.add_argument("--project-version", required=True, help="The project version")
    parser.add_argument(
        "--output", default=Path.cwd(), type=Path, help="Directory to store it in"
    )

    return parser.parse_args()


def main(args):
    download_github_asset(args)


if __name__ == "__main__":
    main(parse_args())

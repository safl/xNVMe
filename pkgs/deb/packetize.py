#!/usr/bin/env python3
"""
These are notes on what to do when producing a Debian source package. The notes
are taken as a self-contained Python script using only included-batteries, such
that it can be conveniently dropped in where needed, such as a GitHUB Action
pipeline.
"""
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
    """Downloads the github asset, unless a local file with the same filename exists"""

    if args.src_filepath.exists():
        log.info(f"'{args.src_filepath}' exists; skipping download")
        return

    url = "/".join(
        [
            "https://github.com",
            args.org_name,
            args.project_name,
            "releases",
            "download",
            f"v{args.project_version}",
            args.src_filename,
        ]
    )

    with urllib.request.urlopen(url) as response:
        with args.src_filepath.open("wb") as content:
            content.write(response.read())


def parse_args():
    """Parse command-line arguments and derive other infot"""

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
        "--output",
        default=Path.cwd() / "builddir",
        type=Path,
        help="Directory to store it in",
    )

    args = parser.parse_args()

    # The local filename of the source-archive
    args.src_filename = f"{args.project_name.lower()}-{args.project_version}.tar.gz"
    args.src_filepath = args.output / args.src_filename

    # This is the root of source archive; after it is extracted
    args.src_root = args.src_filepath.with_name(
        f"{args.project_name.lower()}-{args.project_version}"
    )

    # This is an archive-file which is needed by the debian packaging magic
    args.orig_filename = (
        f"{args.project_name.lower()}_{args.project_version}.orig.tar.gz"
    )
    args.orig_filepath = args.src_filepath.with_name(args.orig_filename)

    return args


def main(args):
    """Script retrieving xNVMe source archive and building the debian source package"""

    run(["rm", "-r", f"{args.output}"])

    args.output.mkdir(parents=True, exist_ok=True)

    # Grab the "released" source-archive
    download_github_asset(args)

    # Create the orig.tar.gz
    run(["cp", f"{args.src_filepath}", f"{args.orig_filepath}"])

    # Extract the source
    run(["tar", "-xzf", f"{args.src_filepath}"], cwd=args.output)

    # Inject the 'debian' folder
    run(["cp", "-r", "debian", f"{args.src_root}"])

    # Now, go ahead and build the package
    run(["dpkg-buildpackage", "-us", "-uc"], cwd=args.src_root)


if __name__ == "__main__":
    main(parse_args())

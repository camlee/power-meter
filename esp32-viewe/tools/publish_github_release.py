#!/usr/bin/env python3
"""Build, sign, tag, push, and create a draft GitHub OTA release."""

import argparse
import base64
import binascii
import configparser
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


PROJECT_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = Path(
    subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"],
        cwd=PROJECT_DIR,
        text=True,
    ).strip()
)
STABLE_SEMVER = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
)
BOARDS = ("meter-viewe", "meter-wroom")


class ReleaseError(RuntimeError):
    pass


def display_command(command):
    return shlex.join(str(value) for value in command)


def run(command, cwd=PROJECT_DIR, **kwargs):
    print("+", display_command(command), flush=True)
    return subprocess.run(command, cwd=cwd, check=True, **kwargs)


def capture(command, cwd=PROJECT_DIR):
    return subprocess.check_output(
        command, cwd=cwd, text=True, stderr=subprocess.DEVNULL
    ).strip()


def capture_optional(command, cwd=PROJECT_DIR):
    result = subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    return result.stdout.strip() if result.returncode == 0 else ""


def normalize_version(value):
    version = value[1:] if value.startswith("v") else value
    if not STABLE_SEMVER.fullmatch(version):
        raise ReleaseError(
            "Version must be stable MAJOR.MINOR.PATCH, optionally prefixed with v."
        )
    return version


def configured_release_repo():
    config = configparser.ConfigParser(interpolation=None)
    config.read(PROJECT_DIR / "platformio.ini")
    repositories = {
        config.get(section, "custom_ota_release_repo")
        for section in ("env:viewe", "env:wroom")
    }
    if len(repositories) != 1:
        raise ReleaseError(
            "Viewe and WROOM must use the same custom_ota_release_repo."
        )
    return repositories.pop()


def require_program(name):
    if shutil.which(name) is None:
        raise ReleaseError("{} was not found on PATH.".format(name))


def require_clean_source():
    tracked = subprocess.run(
        ["git", "diff", "--quiet", "HEAD", "--"], cwd=REPO_ROOT
    )
    staged = subprocess.run(
        ["git", "diff", "--cached", "--quiet", "HEAD", "--"], cwd=REPO_ROOT
    )
    if tracked.returncode or staged.returncode:
        raise ReleaseError(
            "Tracked changes are present. Commit or stash them before releasing."
        )

    project_relative = PROJECT_DIR.relative_to(REPO_ROOT)
    untracked = capture(
        [
            "git",
            "ls-files",
            "--others",
            "--exclude-standard",
            "--",
            str(project_relative),
        ],
        cwd=REPO_ROOT,
    )
    if untracked:
        raise ReleaseError(
            "Untracked files inside {} could affect the build:\n  {}".format(
                project_relative, "\n  ".join(untracked.splitlines())
            )
        )


def current_branch_and_remote(requested_remote):
    branch = capture(["git", "symbolic-ref", "--short", "HEAD"], cwd=REPO_ROOT)
    if not branch:
        raise ReleaseError("Releases cannot be made from a detached HEAD.")

    remote = requested_remote
    if not remote:
        remote = capture_optional(
            ["git", "config", "--get", "branch.{}.remote".format(branch)],
            cwd=REPO_ROOT,
        )
    if not remote:
        remote = "origin"
    if remote == ".":
        raise ReleaseError("The current branch tracks a local branch, not a remote.")

    merge_ref = capture_optional(
        ["git", "config", "--get", "branch.{}.merge".format(branch)],
        cwd=REPO_ROOT,
    )
    remote_branch = (
        merge_ref.removeprefix("refs/heads/") if merge_ref else branch
    )
    return branch, remote, remote_branch


def existing_tag_commit(tag):
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "{}^{{commit}}".format(tag)],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    return result.stdout.strip() if result.returncode == 0 else ""


def expected_assets(directory):
    assets = []
    for board in BOARDS:
        assets.extend(
            [
                directory / "firmware-{}.bin".format(board),
                directory / "manifest-{}.json".format(board),
                directory / "manifest-{}.sig".format(board),
                directory / "ota-{}.json".format(board),
            ]
        )
    return assets


def validate_reusable_assets(directory, version, commit):
    metadata = directory / ".source-commit"
    if not metadata.is_file() or metadata.read_text(encoding="ascii").strip() != commit:
        raise ReleaseError(
            "{} already exists but was not built from the current commit. "
            "Move it aside before retrying.".format(directory)
        )

    for board in BOARDS:
        firmware = directory / "firmware-{}.bin".format(board)
        manifest_path = directory / "manifest-{}.json".format(board)
        signature = directory / "manifest-{}.sig".format(board)
        descriptor_path = directory / "ota-{}.json".format(board)
        for path in (firmware, manifest_path, signature, descriptor_path):
            if not path.is_file():
                raise ReleaseError("Reusable release asset is missing: {}".format(path))

        manifest_bytes = manifest_path.read_bytes()
        try:
            manifest = json.loads(manifest_bytes)
        except (OSError, json.JSONDecodeError) as error:
            raise ReleaseError("Invalid manifest {}: {}".format(manifest_path, error))
        expected_hash = hashlib.sha256(firmware.read_bytes()).hexdigest()
        expected_values = {
            "board": board,
            "firmware_asset": firmware.name,
            "image_size": firmware.stat().st_size,
            "release_tag": "v" + version,
            "sha256": expected_hash,
            "version": version,
        }
        for key, expected in expected_values.items():
            if manifest.get(key) != expected:
                raise ReleaseError(
                    "{} has an unexpected {} value.".format(manifest_path, key)
                )

        try:
            descriptor = json.loads(descriptor_path.read_bytes())
        except (OSError, json.JSONDecodeError) as error:
            raise ReleaseError(
                "Invalid descriptor {}: {}".format(descriptor_path, error)
            )
        try:
            descriptor_manifest = base64.b64decode(
                descriptor["manifest_b64"], validate=True
            )
            descriptor_signature = base64.b64decode(
                descriptor["signature_b64"], validate=True
            )
            detached_signature = base64.b64decode(
                signature.read_text(encoding="ascii").strip(), validate=True
            )
        except (KeyError, ValueError, binascii.Error) as error:
            raise ReleaseError(
                "Invalid signed descriptor {}: {}".format(descriptor_path, error)
            )
        if descriptor_manifest != manifest_bytes:
            raise ReleaseError(
                "{} does not contain its matching manifest.".format(descriptor_path)
            )
        if descriptor_signature != detached_signature:
            raise ReleaseError(
                "{} does not contain its matching signature.".format(descriptor_path)
            )

        with tempfile.NamedTemporaryFile() as signature_file:
            signature_file.write(detached_signature)
            signature_file.flush()
            verified = subprocess.run(
                [
                    "openssl",
                    "pkeyutl",
                    "-verify",
                    "-pubin",
                    "-inkey",
                    str(PROJECT_DIR / "keys" / "ota_signing_public.pem"),
                    "-rawin",
                    "-digest",
                    "sha256",
                    "-sigfile",
                    signature_file.name,
                ],
                input=manifest_bytes,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        if verified.returncode:
            raise ReleaseError(
                "{} does not have a valid OTA signature.".format(manifest_path)
            )


def run_tests():
    run(["pio", "test", "-e", "native"])
    run([sys.executable, "tools/test_ota_crypto.py"])
    run(["npm", "test"], cwd=PROJECT_DIR / "web")
    run(["npm", "run", "check"], cwd=PROJECT_DIR / "web")


def build_assets(version, tag, commit, private_key, final_output):
    final_output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(
        tempfile.mkdtemp(prefix=".{}-".format(tag), dir=final_output.parent)
    )
    try:
        run(
            [
                sys.executable,
                "tools/build_github_release.py",
                version,
                "--private-key",
                str(private_key),
                "--output",
                str(temporary),
            ]
        )
        (temporary / ".source-commit").write_text(commit + "\n", encoding="ascii")
        temporary.replace(final_output)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def release_info(tag, repository):
    result = subprocess.run(
        [
            "gh",
            "release",
            "view",
            tag,
            "--repo",
            repository,
            "--json",
            "isDraft,url",
        ],
        cwd=PROJECT_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if result.returncode:
        return None
    return json.loads(result.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="MAJOR.MINOR.PATCH (a leading v is accepted)")
    parser.add_argument("--remote", help="Git remote to push (default: branch upstream)")
    parser.add_argument("--repo", help="GitHub owner/repository (default: platformio.ini)")
    parser.add_argument(
        "--private-key",
        type=Path,
        default=PROJECT_DIR / "secrets" / "ota_signing_private.pem",
    )
    parser.add_argument(
        "--skip-tests",
        action="store_true",
        help="skip the pre-release test suite",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="validate local inputs and print the release plan without changing anything",
    )
    args = parser.parse_args()

    try:
        version = normalize_version(args.version)
        tag = "v" + version
        repository = args.repo or configured_release_repo()
        branch, remote, remote_branch = current_branch_and_remote(args.remote)
        commit = capture(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT)
        output = PROJECT_DIR / "dist" / tag

        if args.dry_run:
            try:
                require_clean_source()
            except ReleaseError as warning:
                print("Warning: {}".format(warning))
        else:
            require_clean_source()
        for program in ("git", "pio", "openssl", "npm", "gh"):
            require_program(program)
        if not args.private_key.is_file():
            raise ReleaseError(
                "OTA signing key not found: {}".format(args.private_key)
            )

        tagged_commit = existing_tag_commit(tag)
        if tagged_commit and tagged_commit != commit:
            raise ReleaseError(
                "{} already points to a different commit.".format(tag)
            )

        print("Release plan:")
        print("  version:    {}".format(version))
        print("  tag:        {}".format(tag))
        print("  commit:     {}".format(commit))
        print("  source:     {}".format(branch))
        print("  push:       {}/{}".format(remote, remote_branch))
        print("  repository: {}".format(repository))
        print("  output:     {}".format(output))
        if args.dry_run:
            print("\nDry run complete; no tests, builds, tags, pushes, or releases were made.")
            return

        run(["gh", "auth", "status", "--hostname", "github.com"])
        if not args.skip_tests:
            run_tests()

        created_tag = not tagged_commit
        if created_tag:
            run(
                [
                    "git",
                    "tag",
                    "-a",
                    tag,
                    "-m",
                    "Power meter {}".format(tag),
                    commit,
                ],
                cwd=REPO_ROOT,
            )

        try:
            if output.exists():
                validate_reusable_assets(output, version, commit)
                print("Reusing validated release assets in {}".format(output))
            else:
                build_assets(version, tag, commit, args.private_key, output)
        except Exception:
            if created_tag:
                run(["git", "tag", "-d", tag], cwd=REPO_ROOT)
                print("Removed the unpushed tag after the build failure.")
            raise

        run(
            [
                "git",
                "push",
                "--atomic",
                remote,
                "HEAD:refs/heads/{}".format(remote_branch),
                "refs/tags/{0}:refs/tags/{0}".format(tag),
            ],
            cwd=REPO_ROOT,
        )

        assets = [str(path) for path in expected_assets(output)]
        existing_release = release_info(tag, repository)
        if existing_release:
            if not existing_release.get("isDraft"):
                raise ReleaseError(
                    "{} is already published: {}".format(
                        tag, existing_release.get("url", "")
                    )
                )
            run(
                [
                    "gh",
                    "release",
                    "upload",
                    tag,
                    "--repo",
                    repository,
                    "--clobber",
                    *assets,
                ]
            )
        else:
            run(
                [
                    "gh",
                    "release",
                    "create",
                    tag,
                    "--repo",
                    repository,
                    "--draft",
                    "--verify-tag",
                    "--title",
                    tag,
                    "--generate-notes",
                    *assets,
                ]
            )

        info = release_info(tag, repository)
        print("\nDraft release is ready for review:")
        print("  {}".format(info.get("url", "") if info else repository))
        print("Publish the draft only after confirming all eight assets are present.")
    except (ReleaseError, subprocess.CalledProcessError) as error:
        if isinstance(error, subprocess.CalledProcessError):
            sys.exit(
                "Release command failed (exit {}): {}".format(
                    error.returncode, display_command(error.cmd)
                )
            )
        sys.exit(str(error))


if __name__ == "__main__":
    main()

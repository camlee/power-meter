Import("env")
import datetime
import os
import re
import subprocess
import sys

project_dir = env.subst("$PROJECT_DIR")

build_version = os.environ.get("VIEWE_VERSION", "")
if build_version and not re.fullmatch(
        r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
        r"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
        r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?", build_version):
    sys.exit("VIEWE_VERSION must be a valid Semantic Versioning 2.0 version")
if not build_version:
    try:
        described = subprocess.check_output(
            ["git", "describe", "--tags", "--match", "v[0-9]*",
             "--abbrev=7", "--dirty"],
            cwd=project_dir, text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        described = ""
    match = re.fullmatch(
        r"v((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))"
        r"(?:-([0-9]+)-g([0-9a-f]+))?(-dirty)?", described)
    if match:
        base, distance, revision, dirty = match.groups()
        if distance is None:
            build_version = base + ("-dirty" if dirty else "")
        else:
            prerelease = distance + (".dirty" if dirty else "")
            build_version = "{}-{}+g{}".format(base, prerelease, revision)
    else:
        try:
            revision = subprocess.check_output(
                ["git", "rev-parse", "--short=7", "HEAD"],
                cwd=project_dir, text=True, stderr=subprocess.DEVNULL).strip()
        except (OSError, subprocess.CalledProcessError):
            revision = "unknown"
        build_version = "0.0.0-dev+g{}".format(revision)

# Match the Info screen's local date and time presentation.
source_epoch = os.environ.get("SOURCE_DATE_EPOCH")
now = (datetime.datetime.fromtimestamp(int(source_epoch), datetime.timezone.utc)
       if source_epoch else datetime.datetime.now(datetime.timezone.utc))
now = now.astimezone()
build_date = f"{now:%b} {now.day} {now:%Y}"
build_time = now.strftime("%I:%M:%S %p %Z")

# Resolve the include directory
include_dir = env.subst("$PROJECT_INCLUDE_DIR")
if not os.path.exists(include_dir):
    os.makedirs(include_dir)

file_path = os.path.join(include_dir, "build_time.h")

# Write to the header. This updates the file modification timestamp,
# forcing SCons to rebuild only the C++ files that include this header.
with open(file_path, "w") as f:
    f.write(
        "#pragma once\n"
        f"#define BUILD_VERSION \"{build_version}\"\n"
        f"#define BUILD_DATE \"{build_date}\"\n"
        f"#define BUILD_TIME \"{build_time}\"\n"
    )

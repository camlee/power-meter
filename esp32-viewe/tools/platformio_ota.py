"""PlatformIO custom targets for local OTA release creation."""

Import("env")

import os
import sys

project_dir = env.subst("$PROJECT_DIR")
firmware = env.subst("$BUILD_DIR/${PROGNAME}.bin")
release_script = os.path.join(project_dir, "tools", "release.py")
upload_script = os.path.join(project_dir, "tools", "ota_upload.py")
ota_board = env.GetProjectOption("custom_ota_board")
command = '"{}" "{}" --firmware "{}" --board "{}"'.format(
    sys.executable, release_script, firmware, ota_board)
upload_command = '"{}" "{}"'.format(sys.executable, upload_script)

env.AddCustomTarget(
    name="release",
    dependencies="$BUILD_DIR/${PROGNAME}.bin",
    actions=[env.VerboseAction(command, "Signing OTA release")],
    title="Create signed OTA release",
    description="Build firmware.bin and create dist/<version>/manifest.json and manifest.sig.",
)

env.AddCustomTarget(
    name="ota",
    dependencies=[],
    actions=[env.VerboseAction(upload_command, "Uploading signed OTA release")],
    title="Upload signed OTA release",
    description="Upload VIEWE_OTA_RELEASE to VIEWE_OTA_HOST (or VIEWE_OTA_DEVICE).",
)

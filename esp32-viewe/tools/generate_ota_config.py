"""Generate the ignored header that supplies OTA credentials and public key."""

Import("env")

import os
import re


def read_env(path):
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path, encoding="utf-8") as source:
        for line in source:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip().strip('"').strip("'")
    return values


project_dir = env.subst("$PROJECT_DIR")
env_values = read_env(os.path.join(project_dir, ".env"))
token = os.environ.get("VIEWE_OTA_TOKEN") or env_values.get("VIEWE_OTA_TOKEN", "")

try:
    build_header = open(os.path.join(env.subst("$PROJECT_INCLUDE_DIR"), "build_time.h"), encoding="utf-8").read()
    match = re.search(r'^#define BUILD_VERSION "([^"]+)"$', build_header, re.MULTILINE)
    firmware_version = match.group(1) if match else "dev"
except OSError:
    firmware_version = "dev"
board_id = os.environ.get("VIEWE_OTA_BOARD") or env_values.get("VIEWE_OTA_BOARD", "meter")
public_key_path = os.path.join(project_dir, "keys", "ota_signing_public.pem")
try:
    with open(public_key_path, encoding="ascii") as source:
        public_key = source.read()
except FileNotFoundError:
    public_key = ""

header = os.path.join(env.subst("$PROJECT_INCLUDE_DIR"), "ota_public_key.h")
escaped = token.replace("\\", "\\\\").replace('"', '\\"')
escaped_public_key = public_key.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
escaped_version = firmware_version.replace("\\", "\\\\").replace('"', '\\"')
escaped_board = board_id.replace("\\", "\\\\").replace('"', '\\"')
with open(header, "w", encoding="utf-8") as output:
    output.write("#pragma once\n")
    output.write("// Generated from .env and keys/; do not commit.\n")
    output.write('#define OTA_SHARED_TOKEN "{}"\n'.format(escaped))
    output.write('#define OTA_SIGNING_PUBLIC_KEY_PEM "{}"\n'.format(escaped_public_key))
    output.write('#define OTA_FIRMWARE_VERSION "{}"\n'.format(escaped_version))
    output.write('#define OTA_BOARD_ID "{}"\n'.format(escaped_board))

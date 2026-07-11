Import("env")
import datetime
import os

# Keep a local, monotonically increasing build version. The counter is ignored
# by Git so a normal build never dirties the checkout.
project_dir = env.subst("$PROJECT_DIR")
counter_path = os.path.join(project_dir, ".build_number")
try:
    with open(counter_path, "r") as counter_file:
        build_number = int(counter_file.read().strip()) + 1
except (OSError, ValueError):
    build_number = 1
with open(counter_path, "w") as counter_file:
    counter_file.write(str(build_number) + "\n")

build_version = "0.0.{}".format(build_number)
# Match the Info screen's date followed by time presentation.
now = datetime.datetime.now()
build_date = now.strftime("%b %d %Y")
build_time = now.strftime("%I:%M:%S %p")

# Resolve the include directory
include_dir = env.subst("$PROJECT_INCLUDE_DIR")
if not os.path.exists(include_dir):
    os.makedirs(include_dir)

file_path = os.path.join(include_dir, "build_time.h")

# Write to the header. This updates the file modification timestamp,
# forcing SCons to rebuild only the C++ files that include this header.
with open(file_path, "w") as f:
    f.write(f'''
        #pragma once
        #define BUILD_VERSION "{build_version}"
        #define BUILD_DATE "{build_date}"
        #define BUILD_TIME "{build_time}"
        ''')

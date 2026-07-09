Import("env")
import datetime
import os

# Generate a timestamp string
build_time = datetime.datetime.now().strftime("%b %d %Y %H:%M:%S")

# Resolve the include directory
include_dir = env.subst("$PROJECT_INCLUDE_DIR")
if not os.path.exists(include_dir):
    os.makedirs(include_dir)

file_path = os.path.join(include_dir, "build_time.h")

# Write to the header. This updates the file modification timestamp,
# forcing SCons to rebuild only the C++ files that include this header.
with open(file_path, "w") as f:
    f.write(f'#pragma once\n#define BUILD_TIME "{build_time}"\n')

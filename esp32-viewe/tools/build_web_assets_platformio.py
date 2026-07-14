Import("env")

import subprocess
import sys
from pathlib import Path


project = Path(env.subst("$PROJECT_DIR"))
subprocess.run([sys.executable, str(project / "tools" / "build_web_assets.py")], check=True)

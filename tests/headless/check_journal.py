#!/usr/bin/env python3
"""Compile real deterministic journal storage, failure injection and calendar tests."""
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="zhaoxi-journal-") as temp:
 executable=Path(temp)/"test_journal"
 subprocess.run(["cc","-std=c11","-D_POSIX_C_SOURCE=200809L","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-fno-omit-frame-pointer","-I"+str(r/"tests/headless/include"),"-I"+str(r/"app/zhaoxi"),'-DAGENT_DATA_DIR="'+str(Path(temp)/"data")+'"',str(r/"app/zhaoxi/journal.c"),str(r/"tests/headless/test_journal.c"),"-Wl,--wrap=write","-Wl,--wrap=read","-Wl,--wrap=fsync","-Wl,--wrap=close","-pthread","-o",str(executable)],check=True)
 subprocess.run([str(executable)],check=True)

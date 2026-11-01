#!/usr/bin/env python3
"""Check the headless profile and compile its command entry against a patched Agent."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def run(args, cwd=ROOT):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, args)) + "\n" + result.stdout + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--agent-root", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--arm-prefix")
    parser.add_argument("--cjson-root", type=Path)
    options = parser.parse_args()
    manifest = ET.parse(ROOT / "contest2026_416_dianzinongmingong.xml")
    for link in manifest.iter("linkfile"):
        if "zhaoxi_ui" in str(link.attrib):
            raise ValueError("screen UI still linked into active build")
        if not (ROOT / link.attrib["src"]).exists():
            raise ValueError("missing manifest source: " + link.attrib["src"])
    assets = ROOT / "assets/agent_deploy"
    if json.loads((assets / "cron.json").read_text(encoding="utf-8-sig"))["jobs"]:
        raise ValueError("factory scheduled jobs must be opt-in")
    for path in assets.glob("*.md"):
        if path.name == "README.md":
            continue
        text = path.read_text(encoding="utf-8-sig")
        if "/data/ai_agent/" in text or "/data/agent/" in text:
            raise ValueError("hard-coded data root in " + path.name)
        if "{{AGENT_DATA_DIR}}/REMINDER.md" in text:
            raise ValueError("active product asset still depends on screen mailbox")
    note = (assets / "note-taker.md").read_text(encoding="utf-8")
    reminder = (assets / "reminder.md").read_text(encoding="utf-8")
    if "write_file" not in note or "cron_add" not in reminder or "voice" not in reminder:
        raise ValueError("core tool contracts missing")
    report = {"profile": "passed", "board_operations": False}
    if options.agent_root:
        agent = options.agent_root.resolve()
        output = ROOT / "out/headless"
        output.mkdir(parents=True, exist_ok=True)
        include = ["-I" + str(ROOT / "tests/headless/include"), "-I" + str(agent / "src")]
        flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror"]
        sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        source = ROOT / "app/zhaoxi/zhaoxi_main.c"
        obj = output / "zhaoxi_main.o"
        run([options.cc, *flags, *sanitizers, *include, "-Dmain=zhaoxi_main", "-c", str(source), "-o", str(obj)])
        executable = output / "test_commands"
        run([options.cc, *flags, *sanitizers, *include, str(obj), str(ROOT / "app/zhaoxi/todo_store.c"),
             str(ROOT / "app/zhaoxi/journal.c"), str(agent / "src/core/message_bus.c"),
             str(ROOT / "app/zhaoxi/local_intent.c"),
             str(ROOT / "tests/headless/test_commands.c"), "-Wl,--wrap=time", "-pthread", "-o", str(executable)])
        report["commands"] = run([str(executable)]).strip().splitlines()[-1]
        todo_test = output / "test_todo_store"
        run([options.cc, *flags, *sanitizers, "-I" + str(ROOT / "app/zhaoxi"),
             str(ROOT / "app/zhaoxi/todo_store.c"), str(ROOT / "tests/headless/test_todo_store.c"),
             "-Wl,--wrap=fsync", "-Wl,--wrap=rename", "-Wl,--wrap=fwrite", "-pthread", "-o", str(todo_test)])
        report["local_todo"] = run([str(todo_test)]).strip()
        shutdown_test = output / "test_bus_shutdown"
        run([options.cc, *flags, *sanitizers, *include, str(agent / "src/core/message_bus.c"),
             str(ROOT / "tests/headless/test_bus_shutdown.c"), "-pthread", "-o", str(shutdown_test)])
        run([str(shutdown_test)])
        report["shutdown"] = "passed: 40 teardown/re-init cycles with concurrent producers or consumers"
        lifecycle_source = agent / "src/infra/task_lifecycle.c"
        if lifecycle_source.is_file():
            lifecycle_test = output / "test_lifecycle"
            run([options.cc, *flags, *sanitizers, "-I" + str(agent / "src"),
                 str(lifecycle_source), str(ROOT / "tests/headless/test_lifecycle.c"),
                 "-pthread", "-o", str(lifecycle_test)])
            report["worker_lifecycle"] = run([str(lifecycle_test)]).strip()
        for core_only in (True, False):
            # The native loader uses 128-byte paths; use a short runtime root.
            with tempfile.TemporaryDirectory(prefix="zhaoxi-skills-") as temporary:
                data = Path(temporary) / "data"
                executable = Path(temporary) / "test_skills"
                profile_flags = ["-DCONFIG_EXAMPLES_ZHAOXI=1"] if core_only else []
                run([options.cc, *flags, *sanitizers, *include, *profile_flags,
                     '-DAGENT_DATA_DIR="' + str(data) + '"',
                     str(agent / "src/tools/skill_loader.c"), str(ROOT / "tests/headless/test_skills.c"),
                     "-o", str(executable)])
                run([str(executable)])
        report["skill_profile"] = "passed: core-only discovery, legacy file exclusion, deployed asset preservation, standard profile and summary bounds"
        cjson = options.cjson_root or agent.parents[1] / "apps/netutils/cjson/cJSON"
        if (cjson / "cJSON.c").is_file():
            with tempfile.TemporaryDirectory(prefix="zhaoxi-cron-") as temporary:
                data = Path(temporary) / "data"
                executable = Path(temporary) / "test_cron"
                run([options.cc, *flags, *sanitizers, *include,
                     "-include", str(ROOT / "tests/headless/cron_support.h"),
                     "-I" + str(cjson), '-DAGENT_DATA_DIR="' + str(data) + '"',
                     str(agent / "src/infra/cron_service.c"),
                     str(ROOT / "tests/headless/test_cron_persistence.c"),
                     str(cjson / "cJSON.c"), "-pthread", "-lm", "-o", str(executable)])
                report["cron_persistence"] = run([str(executable)]).strip().splitlines()[-1]
            with tempfile.TemporaryDirectory(prefix="zhaoxi-reminder-") as temporary:
                data = Path(temporary) / "data"
                executable = Path(temporary) / "test_reminder"
                run([options.cc, *flags, *sanitizers, *include,
                     "-I" + str(ROOT / "app/zhaoxi"), "-DCONFIG_EXAMPLES_ZHAOXI=1",
                     "-I" + str(agent / "include"),
                     "-DCONFIG_ZHAOXI_BES_AUDIO=1", "-include", str(ROOT / "tests/headless/cron_support.h"),
                     "-I" + str(cjson), '-DAGENT_DATA_DIR="' + str(data) + '"',
                     str(ROOT / "app/zhaoxi/reminder.c"), str(agent / "src/infra/cron_service.c"),
                     str(agent / "src/core/message_bus.c"), str(ROOT / "tests/headless/test_reminder.c"),
                     str(cjson / "cJSON.c"), "-Wl,--wrap=time", "-pthread", "-lm", "-o", str(executable)])
                report["local_reminder"] = run([str(executable)]).strip().splitlines()[-1]
        else:
            report["cron_persistence"] = "not run: cJSON source missing; pass --cjson-root"
        if options.arm_prefix:
            target = output / "zhaoxi_arm.o"
            run([options.arm_prefix + "gcc", "-mthumb", "-mcpu=cortex-m55", *flags, *include,
                 "-Dmain=zhaoxi_main", "-c", str(source), "-o", str(target)])
            report["arm_object"] = "compiled; not a complete flashable firmware"
    (ROOT / "out").mkdir(exist_ok=True)
    (ROOT / "out/headless_checks.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

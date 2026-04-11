#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Run the EEVDF core with a deterministic thread/clock/queue test fixture.

The functions are extracted from sched_eevdf.c, so the tests exercise the
kernel implementation.  The fixture supplies kernel services, not another
scheduler.  This does not replace a kernel build or SMP workload testing.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


FUNCTIONS = """
scale_delta calc_delta_fair thread_is_fair thread_runq tdq_fair_weight
tdq_avg_vruntime entity_eligible entity_update_deadline entity_set_weight
nice_to_pri avg_vruntime_add avg_vruntime_sub update_min_vruntime
eevdf_tree_enqueue eevdf_tree_dequeue tdq_runq_add tdq_runq_rem
tdq_load_add tdq_load_rem tdq_setlowpri tdq_add sched_shouldpreempt
sched_setpreempt tdq_steal tdq_move tdq_notify sched_switch_migrate
update_clock update_curr entity_save_lag place_entity pick_eevdf tdq_choose
check_preempt_eevdf tdq_need_resched sched_thread_change sched_thread_priority
sched_eevdf_rr_interval sched_eevdf_class sched_eevdf_nice
sched_eevdf_lend_prio sched_eevdf_unlend_prio sched_eevdf_lend_user_prio
sched_pctcpu_update sched_eevdf_pctcpu sched_eevdf_prio
sched_eevdf_user_prio sched_eevdf_userret_slowpath sched_eevdf_choose
sched_eevdf_clock sched_eevdf_rem sched_eevdf_wakeup
""".split()


def extract(source, name):
    match = re.search(r"\n" + re.escape(name) + r"\(", source)
    if match is None:
        raise ValueError(f"Missing scheduler function: {name}")
    start = source.rfind("\nstatic ", 0, match.start()) + 1
    brace = source.index("{", match.end())
    # These functions do not contain brace characters in string literals.
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:brace].strip() + ";\n", source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[2]
    source = (root / "sys/kern/sched_eevdf.c").read_text()
    nodes = source[source.index("#define\tRB_RED"):
                   source.index("#define\tEEVDF_BASE_SLICE_NS")]
    constants = source[source.index("#define\tEEVDF_BASE_SLICE_NS"):
                       source.index("struct td_sched {")]
    entity = source[source.index("struct td_sched {"):
                    source.index("_Static_assert(sizeof(struct thread)")]
    tree = source[source.index("#define\tTS_FROM_RB_NODE"):
                  source.index("/*\n * tdq - per processor")]
    queue = source[source.index("struct tdq {"):
                   source.index("/* Idle thread states. */")]
    functions = [extract(source, name) for name in FUNCTIONS]
    fixture = (here / "eevdf_test.c").read_text()
    fixture = fixture.replace("/* EEVDF_TYPES */", nodes + constants + entity + queue)
    fixture = fixture.replace("/* EEVDF_TREE */", tree)
    fixture = fixture.replace("/* EEVDF_FUNCTIONS */",
                              "".join(proto for proto, _ in functions) +
                              "\n".join(body for _, body in functions))
    with tempfile.TemporaryDirectory(prefix="sched-eevdf-") as tmp:
        tmp = Path(tmp)
        (tmp / "priority.h").write_text((root / "sys/sys/priority.h").read_text())
        (tmp / "test.c").write_text(fixture)
        command = shlex.split(os.environ.get("CC", "cc"))
        command += ["-std=gnu11", "-O1", "-g", "-Wall", "-Werror",
                    "-Wno-unused-function", "-Wno-unused-variable"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command + [str(tmp / "test.c"), "-o", str(tmp / "test")],
                       check=True)
        subprocess.run([str(tmp / "test")], check=True)


if __name__ == "__main__":
    main()

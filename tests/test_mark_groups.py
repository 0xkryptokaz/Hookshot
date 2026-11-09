"""a64_lift.mark_groups: the page marks of a straight-line run of stores through one base register shared by one A64_MARK
after the group's last store, and every case where a store must keep its own marks.

python -I -B tests/test_mark_groups.py
"""
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "lift"))
import a64_lift  # noqa: E402

HEAD = "void f_80000000(GuestContext *A64_NOESC c) {"


def run(*body):
    return a64_lift.mark_groups("\n".join((HEAD,) + body + ("}",))).split("\n")[1:-1]


class MarkGroupsTests(unittest.TestCase):
    def test_a_group_shares_one_mark_before_the_call(self):
        out = run("    recomp_store64(c,c->x[19] + 8,c->x[1]);",
                  "    recomp_store32(c,c->x[19] + 16,c->x[2]);",
                  "    { uint64_t _b=c->x[19]; int64_t _o=24; recomp_stp64(c,(_b+_o),c->x[3],c->x[4]); }",
                  "    c->x[0]=recomp_load32(c,c->x[20] + 0);",
                  "    TAILCALL(f_80000100);")
        self.assertEqual(out, ["    recomp_store64_sp(c,c->x[19] + 8,c->x[1]);",
                               "    recomp_store32_sp(c,c->x[19] + 16,c->x[2]);",
                               "    { uint64_t _b=c->x[19]; int64_t _o=24; recomp_stp64_sp(c,(_b+_o),c->x[3],c->x[4]); }",
                               "    c->x[0]=recomp_load32(c,c->x[20] + 0);",
                               "    A64_MARK((c->x[19] + (uint64_t)(int64_t)(8)), 32);",   # [8, 40): every byte the group wrote
                               "    TAILCALL(f_80000100);"])

    def test_a_write_of_the_base_marks_before_it(self):
        out = run("    recomp_store64(c,c->x[8] + 0,c->x[1]);",
                  "    recomp_store64(c,c->x[8] + 8,c->x[2]);",
                  "    c->x[8]=c->x[8] + 16;",
                  "    recomp_store64(c,c->x[8] + 0,c->x[3]);",
                  "    a64_trap(c, 1);")
        self.assertEqual(out[2], "    A64_MARK((c->x[8] + (uint64_t)(int64_t)(0)), 16);")
        self.assertEqual(out[3], "    c->x[8]=c->x[8] + 16;")
        self.assertEqual(out[4], "    recomp_store64(c,c->x[8] + 0,c->x[3]);")   # (alone after the write: its own marks)

    def test_every_run_end_marks_before_it(self):
        for end in ("L_80000010:", "if (!c->z) { goto L_80000010; }", "    recomp_barrier(c);", "    a64_svc(c, 0x60);",
                    "    a64_icall(c, c->x[16]);", "    __atomic_fetch_add((uint32_t *)A64_PTR(c->x[2]), 1, __ATOMIC_SEQ_CST);",
                    "    f_80001000(c);", "    return;"):
            out = run("    recomp_store32(c,c->x[21] + 0,c->x[1]);", "    recomp_store32(c,c->x[21] + 4,c->x[2]);", end,
                      "    recomp_store32(c,c->x[21] + 8,c->x[3]);")
            self.assertEqual(out[2], "    A64_MARK((c->x[21] + (uint64_t)(int64_t)(0)), 8);", end)
            self.assertEqual(out[3], end)

    def test_stores_that_keep_their_own_marks(self):
        cases = [
            # (a conditional store: it may not run)
            ("    if (c->z) recomp_store32(c,c->x[19] + 0,c->x[1]);", "    recomp_store32(c,c->x[19] + 4,c->x[2]);"),
            # (more than 4 KB apart: more than two pages)
            ("    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    recomp_store64(c,c->x[19] + 5000,c->x[2]);"),
            # (a line that also writes a register)
            ("    recomp_store64(c,c->x[19] + 0,c->x[1]); c->x[1]=0;", "    recomp_store64(c,c->x[19] + 8,c->x[2]); c->x[2]=0;"),
        ]
        for body in cases:
            out = run(*body, "    TAILCALL(f_80000100);")
            self.assertFalse(any("A64_MARK" in l or "_sp(" in l for l in out), body)

    def test_bases_group_apart_and_sp_is_left_alone(self):
        out = run("    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    recomp_store64(c,c->x[20] + 0,c->x[2]);",
                  "    recomp_store64(c,c->x[19] + 8,c->x[3]);", "    recomp_store64(c,c->x[20] + 8,c->x[4]);",
                  "    recomp_store64(c,c->x[31] + 8,c->x[5]);", "    TAILCALL(f_80000100);")
        marks = [l for l in out if "A64_MARK" in l]
        self.assertEqual(sorted(marks), ["    A64_MARK((c->x[19] + (uint64_t)(int64_t)(0)), 16);",
                                         "    A64_MARK((c->x[20] + (uint64_t)(int64_t)(0)), 16);"])
        self.assertIn("    recomp_store64(c,c->x[31] + 8,c->x[5]);", out)

    def test_no_group_leaves_the_text_as_it_was(self):
        body = ("    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    TAILCALL(f_80000100);")
        text = "\n".join((HEAD,) + body + ("}",))
        self.assertEqual(a64_lift.mark_groups(text), text)


if __name__ == "__main__":
    unittest.main()

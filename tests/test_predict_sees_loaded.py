"""The prediction sees the programs the preparation has read in.

predict_pipelines (gpu/vk/vk_predict.cpp) learns each pipeline note's vertex layout and targets from the kept program the note
names, through program_cache_each, which listed the records of disk_records. program_cache_load and program_cache_load_some take a
key's records out of disk_records as they read the program in, and a first start's preparation reads in every program the state
list names (state_list_prepare) just before the prediction: no note found its program, and every new build logged "predicted 0
pipelines from 1261 notes for 0 program variants". The records taken out are kept in disk_taken, and program_cache_each lists both.
This checks the source: both take-out sites keep what they take, and program_cache_each walks disk_taken too.
python -I -B tests/test_predict_sees_loaded.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(text, marker):
    start = text.index(marker)
    at = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    return text[start:at]


def main():
    src = (ROOT / "gpu" / "vk" / "vk_shader.cpp").read_text(encoding="utf-8")
    load = body(src, "void program_cache_load(uint64_t key,")
    some = body(src, "size_t program_cache_load_some(")
    each = body(src, "void program_cache_each(")
    if "disk_records.erase(" not in load or "disk_taken[" not in load:
        raise SystemExit("program_cache_load takes records out of disk_records without keeping them for the prediction")
    if "disk_records.erase(" not in some or "disk_taken[" not in some:
        raise SystemExit("program_cache_load_some takes records out of disk_records without keeping them for the prediction")
    if "disk_taken" not in each:
        raise SystemExit("program_cache_each lists disk_records alone: the programs read in already never reach the prediction")
    print("PASS: the prediction sees the programs read in already")


if __name__ == "__main__":
    main()

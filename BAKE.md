# BAKE — offline NVN2 shader bake (developer tooling)

Branch `bake-merge` from `main` 15ec28d. Merges the four `bake-*` race
entries into one shippable, CPU-only, repo-only toolset: ROM/container
enumeration → offline seed-to-SPIR-V translation (no Vulkan device) → bank
emission (portable manifest **and** the live `hookshot_programs.bin` format)
→ read-back verify. No game files, no GPU, no pushes; outputs stay in
`work/` (gitignored) beside the repo, never committed.

Status: **working end-to-end, all tests pass** (results in §7).

## 0. What landed (best-of-breed)

| file | from | what it is |
|---|---|---|
| `gpu/shaderbatch.cpp` | bake-mimo c60fd1d (verbatim) | offline translator: seed → SPIR-V + exact runtime program keys (graphics/compute/task/mesh), no Vulkan device |
| `nx2/bake_mimo.py` | bake-mimo c60fd1d (+1 probe path, see below) | driver: `enumerate` / `translate` / `bank` / `all`; builds shaderbatch against the vendored recompiler, objects cached under the output dir |
| `nx2/bake.py` | bake-spark (manifest + `verify` + stamp-trap tests) | `bake` (seed → combo manifest `bake_bank.json`, resume cursor), `translate` (shaderbatch wrapper merging exact keys into the manifest), `verify` (portable read-back), `stamp`, `census` argv printer |
| `nx2/bake_ds/` (`romenum.py`, `bake_ds.cpp`, `build.sh`, `bake_all.py`, `fixture.py`) | bake-deepseek 54a9737 (verbatim) | container enumeration incl. zstd+dictionaries via libzstd; offline translator + **live-bank writer** (`hookshot_programs.bin` with full `disk_stamp`) + `verify` (a copy of the game's `program_cache_load` reader); end-to-end `bake_all.py` |
| `nx2/bake_glm/lower_probe.cpp` | bake-glm | Ampere encoding probe: hand-built EXIT / MOV.imm through the real `LowerToMaxwell` |
| `tests/test_bake_skeleton.py` | bake-spark | manifest determinism/dedupe/resume, exact-key translate + SPIR-V, stamp-trap, portable verify |
| `tests/test_bake_mimo.py` | bake-mimo c60fd1d | fixture archives → enumerate → translate → bank, FNV round-trip, `spirv-val` on every module |
| `tests/test_bake_ds.py` | bake-deepseek | census parity, Python key mirror, translate + 32 modules + bank read-back (needs `--tool`) |
| `BAKE.md` | this file | the single merged doc (replaces `BAKE-SPARK.md`, `BAKE-MIMO.md`, `BAKE-DEEPSEEK.md`, `BAKE-GLM.md`) |

One-line change vs the sources: `nx2/bake_mimo.py:spirv_headers` also
probes `<repo>/buildext/cpm` (the in-tree CPM cache;
`spark#1` below). `nx2/bake.py`/`tests/test_bake_skeleton.py` reference the
mesh-capable translator (c60fd1d) and its status vocabulary. Everything else
is verbatim from its source branch.

Scope: developer tooling only. No `gpu/vk/*.cpp` hot path is touched
(`git diff main..bake-merge -- gpu/vk` is empty) and no default game behavior
changes — the game never calls this code; it consumes the artifacts on a
Vulkan machine (`prepare_run`/`precompile_run`) exactly as it consumes the
builder's seed today.

## 1. Background (why an offline bake is the same path)

Three ordered steps already run inside `started_up()`
(`gpu/vk/vk_draw.cpp:520-543`): `prepare_run` (translate every seed program,
`:5423`), `predict_run` (`:5653`), `precompile_run` (`:5537`). The seed is
`hookshot_shaders.bin` (`HKSEED01`, `nx2/shader_census.py:19-21,289-294`),
read by `seed_open` (`vk_shader.cpp:1844`) and translated by `seed_program`
(`:1893`) with a **blank `NvnState`** — the translator is already driven with
no game state along this path, so the offline tools mirror it exactly.

Identity (the whole bake is keyed on this):

| key | where | covers |
|---|---|---|
| stage `code_key` | `vk_shader.cpp:99-103,135-137` | FNV-1a 64 over `control[0,0xd8)` then `code[0,end)` |
| compute program key | `:1698` | the stage's `code_key` verbatim |
| graphics program key | `:733-743` (`graphics_key`) | `vs.key ^ fs.key*31`, FNV-mixed with used-generic attribute types + written colour types |
| mesh program key | `:744-752` (`mesh_key`) | `FNV("mesh")` + task/fs keys + primitive SPH + written types |
| variant | `:642-647` (`answers_hash`) | per texture/image answer actually bound |
| disk stamp | `:837-864`, `nx2/build.py:691-699` | translator SHA-1 + depth bit + ABI sizes + env allowlist |

Deduped on `code_key` (not the census's advisory SHA-1: `seed_open`
recomputes FNV either way, `:1872`), a stage is the same two runs of bytes
from ROM archives and from guest RAM at draw time — so a ROM miner is exact.

## 2. Running it

All three drivers are independent entry points over the same seed format;
none reads the game folder unless `--romfs` points at a player dump, and
outputs go under `--out`/`--bank` (default `work/...`, gitignored):

```sh
# MiMo driver: ROM -> seed + manifest -> SPIR-V -> portable bank
python3 -I nx2/bake_mimo.py all --romfs <dump>/romfs --out work/bake
python3 -I nx2/bake_mimo.py enumerate --romfs <dump>/romfs --out work/bake
python3 -I nx2/bake_mimo.py translate --out work/bake [--limit N]
python3 -I nx2/bake_mimo.py bank --out work/bake

# Spark manifest driver: seed -> combos -> portable manifest -> verify
python3 nx2/bake.py bake --seed <hookshot_shaders.bin> --bank <dir> [--resume]
python3 nx2/bake.py translate --seed <hookshot_shaders.bin> --bank <dir>
python3 nx2/bake.py verify --bank <dir>
python3 nx2/bake.py stamp

# DeepSeek driver: tree -> seed -> live bank -> verify (needs build.sh first)
nx2/bake_ds/build.sh                       # -> /tmp/opencode/ds/bake_ds
python3 nx2/bake_ds/bake_all.py all --romfs <dir> --out <dir> --tool /tmp/opencode/ds/bake_ds
python3 nx2/bake_ds/bake_all.py verify --romfs <seed-dir> --out <bank-dir> --tool <tool>

# Ampere encoding probe (needs a C++23 compiler + SPIR-V headers; cf. bake_mimo)
# hand-built EXIT / MOV.imm -> real LowerToMaxwell; exit 0 = all lowered
```

Toolchains: shaderbatch needs a C++23 compiler (`g++`/`clang++`, `-std=c++23`
or `-std=c++2b`) and the SPIR-V headers (`HOOKSHOT_SPIRV_HEADERS`, else the
`buildext/cpm` cache). bake_ds additionally needs the Vulkan headers plus
boost/fmt (same CPM externals the game build uses; `EXT=...` overrides).

Translator stamp (all three agree): `56b84e3925680030` on this tree. The
formula is `sorted(translator-tree) + [vk_shader.cpp, vk_bank.cpp,
vk_state.h, vk_api.h]` appended **after** the sort (`nx2/build.py:691-699`);
see §6 trap #4.

## 3. What each leg does

**Enumerate** (`bake_mimo.enumerate_romfs` / `romenum.enumerate_tree`):
walks `Shader/`, `Layout2Shader/`, `Lib/`, `Movie/`, `Effect/` through BNSH,
BAHS, NDSR, NDVE, `.vsetb` BYML, SARC and zstd `.zs` (dictionaries from
`Pack/ZsDic.pack.zs`); dedupes stages on `code_key`; keeps per-stage
provenance (`file#container#program`). Writes byte-shape-identical
`HKSEED01` plus `manifest.json` (`HKBKM01`) / `enum.json`. Compressed files
without a decoder are *counted and reported*, never silently skipped.

**Translate** (`gpu/shaderbatch.cpp`, built by `bake_mimo.build_tool`):
per stage mirrors `parse_code` (capture → `LowerToMaxwell` → Maxwell words);
per program mirrors `seed_program`'s kind/input-variant choice and the
runtime's `RuntimeInfo` per stage (vertex `vrt`, fragment with the previous
stage's stores + `GuardFragmentOutputs`, compute `RuntimeInfo{}`, task/mesh
via `translate_mesh_into` with `prim=null` exactly as the seed path passes
it); emits SPIR-V per `<progkey>_<stage>.spv` with `stages.tsv`/`programs.tsv`.
Statuses are machine stop-lines: `OK`, `LOWER_ERROR` (+ the refusing opcode),
`TRANSLATE_ERROR`, `EMIT_ERROR`, `HEADER_ONLY` (graphics `len <= 0x80+16`),
`NOT_ATTEMPTED`, `INVALID_STAGE`. No mesh skip: task/mesh registers are
packed from the control sections (`mesh_registers_of`, mirror of
`vk_shader.cpp:1311-1326`) and the pair emits `MeshEXT`/`TaskEXT` modules.

**Bank, portable** (`bake_mimo.make_bank` / `bake.bake_seed`): `bank.json`
(`HKBANK01`: counts, `translator_stamp`, assumptions) + `stages.jsonl` /
`programs.jsonl` + `spv/`; every pipeline row `PENDING_GPU` (pipelines need a
`VkDevice`). `bake.py` adds the combo manifest: per-program input variants
× plausible pipeline states (tight cap, default 8/program), atomic-rename
`bake_bank.json` with a resume cursor (`done_programs`, idempotent), and
`translation.by_program` merging the exact runtime keys where translation ran.

**Bank, live** (`bake_ds translate`): the back end's own
`hookshot_programs.bin` (`kDiskMagic`, full `disk_stamp` incl.
`sizeof(Shader::Info)`/`sizeof(Program)`, stdlib marker, depth bit and env)
plus `bake_report.jsonl` (per-stage descriptor plans, workgroup/shared,
question counts, compute `CsLayout`) and `bake_manifest.json`. `bake_ds
verify` re-reads the file through a copy of `program_cache_load`'s
`Reader`/`get_info` and requires 0 bad records; it verifies *any*
`hookshot_programs.bin`, including one the game wrote.

**Verify, portable** (`bake.verify_bank`): seed sha round-trip, stamp
currency, SPIR-V magic + `spirv-val --target-env vulkan1.3` over every merged
module; writes `verify.json`. The analogue of `bake_ds verify` for the
portable format.

**Probe** (`lower_probe.cpp`): pins the hand-built Ampere encodings the
fixtures rely on (EXIT `w0=0x794d, w2=7<<23`; MOV.imm `0x802|dest<<16`;
MOV.cbuf `(off<<6)|(bank<<22)`) through the real `LowerToMaxwell`.

## 4. Offline assumptions (what is faked, and recorded)

| thing | offline answer | live source |
|---|---|---|
| texture/image variants | unbound: `Color2D`/not-integer; one baseline row per program | `record_answers` on bound state (`:642-677`); `seed_compute` enumerates trials (`:1966-2082`) |
| driver banks c0/c2 | 0, except vertex c0+0x610 = `2047.9375f` (`vk_bank.cpp:405`) | per-draw `driver_bank_word` |
| user cbufs c3..c18 | 0 | bound guest memory |
| `min_ssbo_alignment` | 16 (Vulkan minimum; `--ssbo-align` on bake_ds) | device `dev.props.limits` |
| `convert_depth_mode` | false | `host->depth_minus_w()` |
| colour output types | all Float (`no_types`) | bound render targets |
| mesh `prim` header | null (as the seed path passes it, `:1945`) | bound pass-through geometry program |
| pipelines | `PENDING_GPU` everywhere | `VkDevice` builds (`build_graphics`) |

A bank baked with a different depth bit, env allowlist entry, or C++ ABI
loads-or-refuses whole (`disk_stamp`, `:837-864`) — never misreads. Live
banks are therefore same-toolchain artifacts (builder-integrated or a
player-PC step); the portable bank is the cross-toolchain handoff a GPU step
consumes.

## 5. What was cut, and what the alternatives are

1. **GLM `bake_translate.cpp` / `run_bake.py` / `bake_enumerate.py` /
   `bake_banks.py`** — cut as duplicate translators. `bake_translate.cpp`
   covers compute+graphics with its own JSONL layout and needs
   `<vulkan/vulkan.h>`; `shaderbatch.cpp` covers compute+graphics+task/mesh
   with the exact runtime keys and no Vulkan headers, and both agree on the
   stamp (`56b84e…`) and `min_ssbo_alignment = 16`. Kept from GLM: the
   encoding probe (`lower_probe.cpp`, unique harness value) and its findings
   (static-offset cbuf reads fold under ConstantPropagationPass; the
   `VK_SHADER_STAGE_ALL` bitmask note). Alternative recorded: if a JSONL
   per-emit log is ever preferred to TSV, `bake_glm/bake_translate.cpp`
   (bake-glm branch) is the drop-in.
2. **GLM `bake_banks.py` doc-shape banks** (`c0_<key>.bin` + `banks.json`) —
   cut, superseded by `bake_ds`'s live bank writer (real format + verify).
   Alternative recorded above.
3. **Spark approximate `program_key_skeleton`** — superseded wherever
   translation ran: `translation.by_program[].runtime_key` is the exact
   `graphics_key`/`mesh_key`/compute key. The skeleton key remains only for
   untranslated rows (resume/dedupe before translation).
4. **DeepSeek type-variant enumeration** — not baked: only the plain
   (unbound) variant is written per program (matches `seed_program`, not
   `seed_compute`'s trials). Rationale (§6.3 of the race): on the blank state
   the trials' type-only answers change SPIR-V but not `answers_hash` and are
   dropped by `variants.insert`, while integer toggles are kept — so the
   baseline is nearly all the live path keeps. A follow-up that records
   forced answers into `Program::answers` is the next piece, not this merge.
5. **Mesh `prim` pass-through, texture-variant rows, pipeline notes,
   distribution** — all deliberately out of scope (no game/GPU here); every
   leg marks them (`PENDING_GPU` / `NOT_TRANSLATED` / stop-line statuses)
   instead of faking them.

## 6. Traps the merge preserves (read before touching)

1. **Stamp sort trap**: `sorted(translator-tree)` then the four named files
   **appended** — merging them into the sort yields a plausible-but-wrong
   `158e661374b23b7a`. Guarded by `BakeStampTrapTests`.
2. **Header-only trap**: graphics `len <= 0x80+16` is a header alone
   (`parse_code`, `:175-179`) — tiny single-instruction fixtures report
   `HEADER_ONLY`, they are not mistranslations.
3. **Pipeline keys must fold the input variant** (`seed_program(i/3, i%3)`),
   or dedupe collapses the three bone variants into one.
4. **Resume cursors must retain prior combos** (`old_combos + combos`), not
   just skip done keys.
5. **`spirv_headers` probe list** must include the in-tree `buildext/cpm`
   (this merge's one-line fix); `bake_ds/build.sh` instead probes the
   azahar-build externals with `EXT=` override.
6. **CsLayout fallback** with `ssbo == 0` is base 0 / count 16 / end 0
   (`:1126-1128`), not `end()`.
7. **`bake verify` vs `bake_ds verify`**: the former checks the portable
   manifest, the latter the live bank — both must pass, they check different
   artifacts.

## 7. Test results (this machine, CPU-only, repo + `/tmp` only)

```
tests/test_bake_skeleton.py ............ 8 passed
tests/test_bake_mimo.py ................ PASS (3 programs, 5 stages, 6 modules spirv-val VALID)
tests/test_bake_ds.py .................. PASS (16 programs / 18 slots, 32 modules, 18 records / 0 bad)
python3 -I -B tests/run_all.py bake .... 3 passed, 0 failed, 0 skipped in 38 s
```

`spirv-val --target-env vulkan1.3` VALID on every emitted module;
`MeshShadingEXT`/`TaskEXT` entry points present on the mesh pair;
`spirv-dis` shows `LocalSize 8 1 1` from the control section's workgroup.
`bake_ds` was rebuilt standalone from the committed tree
(`OUT=/tmp/opencode/ds-merge nx2/bake_ds/build.sh` → 163 recompiler
sources, `built .../bake_ds`) and `tests/test_bake_ds.py` passes against
that fresh binary as well as the prebuilt one. The repo runner's bake scope
(`tests/run_all.py bake`: each `test_bake_*.py` in its own process) passes
whole; the runner invokes `test_bake_ds.py` without `--tool`, so its
translate/bank half is covered by the two direct runs above, not by the
runner. The game directory, saves and caches were never read or written; no
GPU was used; nothing game-derived is committed (`work/`, `*.bin` stay
ignored).

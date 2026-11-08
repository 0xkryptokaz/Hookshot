# SPIR-V global halfword load regression

This fixture builds a small `IR::Program`, runs the real shader-info pass and
`EmitSPIRV`, then checks both the emitted instructions and the generated module
with `spirv-val`. It covers signed and unsigned loads at naturally halfword-
aligned offsets 0 and 2, preservation of high guest-address bits, and an access
at byte offset `0xffe` whose helper word base is `0xffc`. A single-storage-
buffer fixture checks that the real global U32 helper computes its logical end
from the original descriptor address plus size, then caps that end with the
actual `OpArrayLength` value before its range comparison. The test evaluates
the emitted helper's membership branch with cbuf and
descriptor inputs: `0x1004` with a two-byte range and 16-byte alignment is a
hit four bytes into the aligned binding; `0x1008` is a miss, and `0x1000` is
the aligned-base control. It also checks a shorter descriptor cap, a zero-length
descriptor, a high-64-bit base, and the existing zero-result behavior when the
profile lacks 64-bit integer support.

The fixture checks the emitted SPIR-V dependency chain; it does not execute a
shader or driver, and does not validate odd guest addresses or AMD hardware. It
tests the SPIR-V backend's lowering contract only.
The game-free workflow checks out the pinned SPIR-V-Headers revision and uses
the system SPIR-V Tools validator.

Reproduce it with Clang in C++20 mode, as in the Linux workflow. The focused
target includes project compatibility headers with a function-local static
inside a `constexpr` assertion macro. GCC rejects that construct in C++20,
while the project's production Linux compiler is Clang in C++20 mode. The
fixture does not establish an unmodified MSVC build.

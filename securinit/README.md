# securinit bootstrap

`build_coreinit.sh` links securinit as a separate x86_64 or arm64 Mach-O. A BSP
coreinit job claims a region through the memory-manager object, loads the image,
and passes only the factory capability, a status-sink capability, and the
existing `kernel_invoke` bootstrap pointer. securinit creates its own Status
object with the factory and invokes it through the same IPC interface; that
object forwards a marker to coreinit's sink. No direct C symbol table or
logger callback crosses the image boundary. The wire method is defined in
`schemas/securinit.capnp`, compiled into const index-based metadata at build
time; the per-image descriptor bridge is wired on entry.

This is **not authorization yet**. The kernel and both stages still run in
ring 0 with raw pointer-valued handles and a factory accepting native handler
addresses. Such a process could bypass any wrapper or read a purported secret.
A real securinit authorization policy requires a trusted loader plus
privilege/isolation for untrusted code, and a factory-controlled wrapping
object that does not hand out its unguarded underlying handle. No security
claim should be made from the current status-marker demonstration.

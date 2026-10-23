# securinit bootstrap

`build_coreinit.sh` links securinit as a separate x86_64 or arm64 Mach-O. A BSP
coreinit job claims a region through the memory-manager object, loads the image,
and passes only the factory capability, a status-sink capability, and the
existing `kernel_invoke` bootstrap pointer. securinit creates Status, Gate,
and Admin objects through the factory. Gate initially denies a report; an
Admin capability kept within the stage enables it, then Gate forwards the
report through Status to coreinit's sink. Coreinit queries Gate's counters
through IPC and sees one denial and one forwarding. No direct C symbol
table or logger callback crosses the image boundary. These interfaces live
in `schemas/securinit.capnp`, compiled into const index-based metadata at
build time; per-image descriptor bridges are wired on entry.

On ARM the wrapper now also allocates a separate 2 MiB guest region and
briefly enters EL0. A real permission fault denies a read of its EL1-only
Admin record. The wrapper also receives EL0 `svc #0` calls at the existing
EL1 vector, rejects an ungranted local endpoint and a request pointer aimed
at privileged Admin memory before dereferencing either, then copies a bounded
flat Cap'n Proto request/response through its trusted Gate capability for the
only granted endpoint. BRK returns to the wrapper in EL1. QEMU checks the
Admin FAR, two SVC exceptions, the forwarded IPC marker, and EL1 return.
This is a one-shot isolated probe, not a long-lived untrusted service. The
x86 wrapper still runs entirely in ring 0.

This is **not full authorization yet**. The privileged kernel and wrapper
stage still hold native handles and handler pointers; only the one tested
EL0 page is isolated and mediated. A general-purpose loader must keep
untrusted code out of privileged mappings, hold Admin/Status handles only
at EL1, and give each wrapped object its own scoped endpoint grants.
A real securinit authorization policy requires a trusted loader plus
privilege/isolation for untrusted code, and a factory-controlled wrapping
object that does not hand out its unguarded underlying handle. Gate/Admin
prove the IPC control path, not that an attacker cannot read the private
Admin handle or bypass Gate by invoking Status directly.

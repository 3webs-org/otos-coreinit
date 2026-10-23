@0xcef18f97329da6b4;

# Stage-2 bootstrap protocol. These are ordinary factory-created objects,
# invoked through the same kernel_invoke path as every other object.
# Gate delegates to Status only after a separate Admin capability enables it.
# Raw pointer handles in ring 0 mean this is NOT a security boundary yet.
interface Status {
  report @0 (value :UInt64) -> ();
}

interface Gate {
  invoke @0 (value :UInt64) -> (forwarded :UInt64);
  getStats @1 (unused :UInt64) -> (denied :UInt64, forwarded :UInt64);
}

interface Admin {
  setAllowed @0 (allowed :UInt64) -> (ok :UInt64);
}

@0xdcb18c714e93a625;

# Wire ABI for coreinit's IPC objects. Every UInt64 occupies one data word
# in the existing flat Cap'n Proto segment. Handles are UInt64 values, not
# Cap'n Proto capability pointers; changing one to a pointer changes the ABI.
# An empty result means the kernel performs no result-shape validation.
# tools/gen-capnp-schemas.py compiles these interfaces into read-only,
# index-based schema_table_t metadata at build time. No schema text or
# parser is linked into the kernel or init image.

interface Plain {
  update @0 (value :UInt64) -> ();
}

# A location-transparent unit of computation. All arguments and results are
# values on the wire; unlike cluster_job_t.ctx, nothing here is an address in
# the submitting machine. Job type 1 is the initial proof (arg0 + arg1).
interface RemoteExecutor {
  run @0 (jobType :UInt64, arg0 :UInt64, arg1 :UInt64) ->
    (ok :UInt64, result :UInt64);
}

# Link discovery and routing envelopes are Cap'n Proto too.  They are carried
# outside kernel_invoke, but use the same generated, versioned word shapes as
# every capability method instead of a private packed-C/network header.
interface NetworkDiscovery {
  announce @0 (magic :UInt64, node :UInt64, service :UInt64,
    capability :UInt64) -> ();
}

interface NetworkRpcEnvelope {
  route @0 (magic :UInt64, versionAndType :UInt64, requestId :UInt64,
    sourceNode :UInt64, destinationNode :UInt64, targetHandle :UInt64,
    methodAndPayloadLength :UInt64,
    responseCapacityAndStatus :UInt64) -> ();
}

interface Disk {
  read @0 (lba :UInt64) -> (
    word00 :UInt64, word01 :UInt64, word02 :UInt64, word03 :UInt64,
    word04 :UInt64, word05 :UInt64, word06 :UInt64, word07 :UInt64,
    word08 :UInt64, word09 :UInt64, word10 :UInt64, word11 :UInt64,
    word12 :UInt64, word13 :UInt64, word14 :UInt64, word15 :UInt64,
    word16 :UInt64, word17 :UInt64, word18 :UInt64, word19 :UInt64,
    word20 :UInt64, word21 :UInt64, word22 :UInt64, word23 :UInt64,
    word24 :UInt64, word25 :UInt64, word26 :UInt64, word27 :UInt64,
    word28 :UInt64, word29 :UInt64, word30 :UInt64, word31 :UInt64,
    word32 :UInt64, word33 :UInt64, word34 :UInt64, word35 :UInt64,
    word36 :UInt64, word37 :UInt64, word38 :UInt64, word39 :UInt64,
    word40 :UInt64, word41 :UInt64, word42 :UInt64, word43 :UInt64,
    word44 :UInt64, word45 :UInt64, word46 :UInt64, word47 :UInt64,
    word48 :UInt64, word49 :UInt64, word50 :UInt64, word51 :UInt64,
    word52 :UInt64, word53 :UInt64, word54 :UInt64, word55 :UInt64,
    word56 :UInt64, word57 :UInt64, word58 :UInt64, word59 :UInt64,
    word60 :UInt64, word61 :UInt64, word62 :UInt64, word63 :UInt64);
}

interface Resource {
  getInfo @0 (unused :UInt64) -> (ownerNode :UInt64, kind :UInt64,
    descriptor :UInt64, capacity :UInt64, syncKind :UInt64, reservedBy :UInt64);
  reserve @1 (requestingJob :UInt64) -> (ok :UInt64);
  releaseReservation @2 (requestingJob :UInt64) -> (ok :UInt64);
  reassign @3 (newOwnerNode :UInt64) -> (ok :UInt64);
}

interface ClusterScheduler {
  joinNode @0 (nodeId :UInt64, arch :UInt64,
    signalTarget :UInt64) -> (nodeHandle :UInt64);
  leaveNode @1 (nodeId :UInt64) -> ();
  requestAllocation @2 (nodeId :UInt64, base :UInt64, size :UInt64,
    kind :UInt64, syncKind :UInt64) -> (ok :UInt64, resourceId :UInt64);
  declareResource @3 (nodeId :UInt64, kind :UInt64, descriptor :UInt64,
    capacity :UInt64, syncKind :UInt64) -> (resourceId :UInt64);
  reassignResource @4 (resourceId :UInt64, newOwnerNode :UInt64) -> (ok :UInt64);
  submitJob @5 (nodeAffinity :UInt64, jobType :UInt64, ctx :UInt64,
    priority :UInt64, resourceId :UInt64, resourceKind :UInt64,
    resourceMinCapacity :UInt64) -> (jobId :UInt64);
  getNextJob @6 (requestingNodeId :UInt64) -> (found :UInt64,
    jobHandle :UInt64, jobType :UInt64, ctx :UInt64);
  reportJobResult @7 (jobHandle :UInt64, result :UInt64, blockId :UInt64) -> ();
  mutexTryLock @8 (mutexId :UInt64, requestingJobId :UInt64) -> (ok :UInt64);
  mutexUnlock @9 (mutexId :UInt64, requestingJobId :UInt64) -> ();
  getResourceInfo @10 (resourceId :UInt64) -> (found :UInt64,
    ownerNode :UInt64, kind :UInt64, descriptor :UInt64,
    capacity :UInt64, syncKind :UInt64);
  allocateSlice @11 (nodeId :UInt64, requestingJobId :UInt64, size :UInt64,
    kind :UInt64, syncKind :UInt64) -> (ok :UInt64, resourceId :UInt64,
    base :UInt64);
  releaseSlice @12 (resourceId :UInt64, requestingJobId :UInt64) -> (ok :UInt64);
  preemptNode @13 (nodeId :UInt64) -> (ok :UInt64);
}

interface Node {
  getInfo @0 (unused :UInt64) -> (nodeId :UInt64, arch :UInt64);
  signal @1 (unused :UInt64) -> ();
}

interface CsMutex {
  tryLock @0 (requestingJobId :UInt64) -> (ok :UInt64, holderJobId :UInt64);
  unlock @1 (requestingJobId :UInt64) -> (ok :UInt64);
}

interface Job {
  getInfo @0 (unused :UInt64) -> (type :UInt64, ctx :UInt64,
    basePriority :UInt64, effectivePriority :UInt64, nodeAffinity :UInt64,
    assignedNode :UInt64, resourceId :UInt64, resourceKind :UInt64,
    resourceMinCapacity :UInt64, boundResourceId :UInt64, blocked :UInt64,
    blockKind :UInt64, blockId :UInt64);
  updateSchedulingState @1 (assignedNode :UInt64, boundResourceId :UInt64,
    blocked :UInt64, blockKind :UInt64, blockId :UInt64,
    effectivePriority :UInt64) -> (ok :UInt64);
}

interface MutexManager {
  create @0 (unused :UInt64) -> (mutexId :UInt64);
  lock @1 (mutexId :UInt64) -> (ok :UInt64);
  unlock @2 (mutexId :UInt64) -> ();
}

interface MemoryManager {
  claim @0 (base :UInt64, size :UInt64, kind :UInt64) -> (ok :UInt64);
  alloc @1 (size :UInt64, kind :UInt64) -> (ok :UInt64, base :UInt64);
  free @2 (base :UInt64) -> (ok :UInt64);
}

interface IrqManager {
  bind @0 (irq :UInt64, target :UInt64) -> ();
}

# Trusted wrapper installation of a lower-EL synchronous-exception handler.
# This object routes hardware exceptions; securinit owns policy and the gate.
interface El0ExceptionRouter {
  bind @0 (handler :UInt64) -> (ok :UInt64);
}

# A trusted x86 wrapper owns the ring-3 entry and invoke override. Coreinit
# only installs the hardware IDT/TSS routes and keeps existing IRQ/#PF gates.
interface Ring3ExceptionRouter {
  bind @0 (invokeEntry :UInt64, exitEntry :UInt64, trustedStack :UInt64,
    faultHook :UInt64) -> (ok :UInt64);
}

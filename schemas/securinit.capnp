@0xcef18f97329da6b4;

# Stage-2 bootstrap protocol. This is an ordinary factory-created object,
# invoked through the same kernel_invoke path as every other object.
# It proves independently-loaded securinit can own code/data and publish
# a capability; it is NOT an authorization/security boundary yet.
interface Status {
  report @0 (value :UInt64) -> ();
}

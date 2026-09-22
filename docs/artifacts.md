# Durable artifact store

LASO stores bounded workspace and result files as immutable, content-addressed
objects. The object identifier is `sha256:<64 lowercase hex characters>` and
the filesystem layout is private to the configured artifact root:

```text
artifact_root/
  objects/ab/cdef...        immutable object bytes
  temp/                     mode-0600 incomplete uploads
```

Uploads and materialization stream in bounded chunks. LASO verifies the
cryptographic digest and byte count before accepting an object or exposing it
to a worker workspace. Finalization uses a temporary file followed by an atomic
rename; equivalent duplicate uploads resolve to the same object. Object
metadata is durable in the configured storage backend and contains logical
name/provenance, not a portable host path.

## Workspace manifests

Small legacy manifests may inline file bytes (version 1). New distributed
workspace manifests use version 2 and contain only safe relative paths,
object IDs, SHA-256 values, and sizes. Staging rejects absolute paths,
traversal, duplicate entries, symlinks, oversized files, oversized totals, and
integrity mismatches. Provider processes receive only the ephemeral staged
directory; the host's absolute path is never portable workflow state.

Returned manifests carry run, `NodeWork`, attempt, worker, and fencing
provenance. The owner verifies every referenced object against that provenance
and digest before accepting the manifest into the durable joined message. A
stale or conflicting `NodeWork` result cannot make its object references
authoritative because the enclosing completion is still protected by the
PostgreSQL fence.

## Multi-instance transfer

The canonical backend is filesystem-backed. For PostgreSQL multi-instance
execution, configure the same trusted reachable artifact root for every
participating instance when such a mount is available. A local per-host root
cannot satisfy remote object materialization by itself.

Where a shared mount is not available, the store owner can enable the scoped
artifact gateway with `artifact_service_port` and a deployment-local bearer
token. A worker sets `artifact_service_url` and the same token. The gateway
supports only authenticated `PUT /api/v1/artifacts` and authenticated
`GET`/`HEAD /api/v1/artifacts/<object-id>` operations; bodies stream through
private temporary files and object IDs are validated before lookup. It is not a
general file server, and PostgreSQL still stores only metadata and references.
Keep the listener loopback/private and use a protected tunnel or network when
crossing hosts. Do not put provider credentials, SSH keys, environment dumps,
or unrelated host paths in a manifest.

## Inspection and cleanup

```sh
laso artifact list --run-id RUN_ID
laso artifact verify
laso artifact gc                 # dry-run report
laso artifact gc --execute       # remove only aged, unreferenced objects
```

`artifact verify` streams all object files and reports invalid objects and
temporary uploads without dumping their contents. Garbage collection is
conservative: durable artifact metadata is treated as a live reference and
incomplete uploads are removed only after the configured grace period. Use a
database backup and filesystem backup policy appropriate to the deployment;
LASO does not provide a general rollback for object deletion.

The store provides bounded at-least-once object publication and one
authoritative fenced workflow completion. It does not claim exactly-once
provider execution, arbitrary remote filesystem access, or an unauthenticated
object-store service/API for untrusted networks. Possession of the deployment
token grants access to the configured artifact namespace, so protect and rotate
it like other service credentials.

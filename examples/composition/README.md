# Offline pipeline composition

These definitions are deterministic and require no model service, network, or
credential. Register referenced revisions before their parents:

```sh
laso pipeline register examples/composition/normalize.yaml
laso pipeline register examples/composition/process.yaml
laso run start process@1 --input '{"value":42}'
```

The `process@1` pipeline invokes `normalize@1`, then runs a local deterministic
function. The `nested-*.yaml` files show the same reusable composition shape as
`nested-a@1 -> nested-b@1 -> nested-c@1`; register them in C-to-A order.

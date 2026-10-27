# Source-save generators

Read before attaching an executable. Examples use `$preview` for the owning checkout's `bin/asset-preview`; see [connections](connections.md) for registration.

When the agent already runs the build, ordinary output watching is sufficient.
To rebuild on source saves, attach one explicit **finite** generator to one view:

```sh
"$preview" add /absolute/project/exports/chair.obj \
  --id project-chair-model --watch /absolute/project/source/chair.json \
  --watch /absolute/project/scripts/build-chair.sh --cwd /absolute/project \
  --exec /absolute/project/scripts/build-chair.sh
```

`--watch` alone never executes a script. Put `--exec` last; argv has no shell
expansion. The wrapper owns its prescribed runtime/container invocation, mounts
and project/GPU locks. Watch source and recipe dependencies, excluding generated
outputs. Publish validated outputs atomically. Do not run the same builder
independently while this view owns it or
attach it to several views. Editors, games and long-lived servers are unsuitable
generators. Source-triggered generation runs only while the view is visible.
Valid intermediate output saves refresh during a run. The timeout defaults to
120 seconds; set `--timeout` explicitly for a known finite job needing more
(max 3600).

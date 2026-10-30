# Release Publishing

For archive build commands, see [Building ODFileSystem](building.md).

Pushing a `v*` tag runs the GitHub draft-release workflow and the separate
Aminet publishing workflow. The draft-release workflow also submits its
existing OS4 archive to OS4Depot, without rebuilding it. OS4Depot receives
`odfilesystem.lha` and `odfilesystem_lha.readme`, replacing the existing
`driver/filesystem/odfilesystem.lha` entry.

Configure the repository Actions secret `OS4DEPOT_PASSPHRASE` with the
passphrase for that existing OS4Depot entry (1-40 characters on one line).
An absent or invalid secret fails the OS4Depot job before any upload;
the GitHub release and Aminet jobs can still complete independently.
The passphrase is added only to a temporary upload readme, never to the
archive or GitHub artifacts.

The upload metadata lives in [`ODFileSystem_OS4.os4depot`](ODFileSystem_OS4.os4depot). The local
composite action in [`.github/actions/os4depot-release`](../.github/actions/os4depot-release) accepts archive,
destination filename, readme template, version, and passphrase inputs.
It follows the [OS4Depot FTP protocol](https://os4depot.net/index.php?function=ftpinfo),
sending the archive first and the readme last. Successful transfer means
submission to OS4Depot; site validation and moderator approval still follow.

To publish an existing tag, run **ODFileSystem Draft Release** manually
from the updated branch and supply that tag. To retry only a failed
OS4Depot upload, rerun its failed job rather than the entire workflow.

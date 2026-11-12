# Saved PCCSCC Docker image

This folder contains a prepared **Linux/amd64** runtime image:
[`pclib-ptdlib-linux-amd64.tar.gz`](pclib-ptdlib-linux-amd64.tar.gz), approximately
67.1 MiB. You need Docker with Linux-container support; no compiler, LLVM/Clang
installation, or Python is needed to run it.

Keep the archive and [`SHA256SUMS`](SHA256SUMS) in a folder named `docker-image`.
Run all commands below from its **parent directory** (the repository root in a
full checkout).

## Check and load

Optional checksum verification in Linux/WSL:

```bash
(cd docker-image && sha256sum -c SHA256SUMS)
```

Or in PowerShell, compare the two hexadecimal hashes (ignoring case):

```powershell
Get-FileHash .\docker-image\pclib-ptdlib-linux-amd64.tar.gz -Algorithm SHA256
Get-Content .\docker-image\SHA256SUMS
```

Load in either shell; Docker reads the compressed archive directly:

```text
docker load --input docker-image/pclib-ptdlib-linux-amd64.tar.gz
```

The image tag restored by the archive is `pclib-ptdlib:local`.

## Run and retain the results

Linux/WSL:

```bash
mkdir -p reports
docker run --rm --pull=never --platform=linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$(pwd)/reports,target=/reports" \
  pclib-ptdlib:local
```

Windows PowerShell, with Docker Desktop using Linux containers:

```powershell
New-Item -ItemType Directory -Force -Path .\reports | Out-Null
$reportDir = (Resolve-Path .\reports).Path

docker run --rm --pull=never --platform=linux/amd64 `
  --mount "type=bind,source=$reportDir,target=/reports" `
  pclib-ptdlib:local
```

The default nested-if example produces 6 complete path objectives (5 SAT,
1 UNSAT). Libraries are saved as `reports/pclib/nested_if_pclib.json` and
`reports/ptdlib/nested_if_ptdlib.json`, alongside inspection outputs. These are
constraint-analysis results, not hardware timing measurements.

See [Docker usage](../docs/DOCKER.md) for your own C files, saved-PClib queries,
image metadata, and runtime validation. The image is a runnable artifact, not the
full native build/test environment.

## Repository upload

This archive is intended for Git commit/push. At 67.1 MiB it exceeds GitHub's
25 MiB browser-upload limit but is below its 100 MiB Git file limit; a large-file
warning above 50 MiB is expected. See
[GitHub's limits](https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-large-files-on-github).

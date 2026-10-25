"""Package a committed source tree (including pinned submodules) and built distribution.

Run tools/package.ps1 first, then:
    python tools/release_assets.py --version 1.1.0 --dist dist-1.1.0
No credentials, local QA photos, build tree or Git metadata enter the source ZIP.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def git(*args, cwd=ROOT):
    return subprocess.check_output(['git', *args], cwd=cwd)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def archive_into(output, repo, prefix=''):
    with tarfile.open(fileobj=io.BytesIO(git('archive', '--format=tar', 'HEAD', cwd=repo))) as source:
        for entry in source:
            if entry.isfile():
                output.writestr(prefix + entry.name, source.extractfile(entry).read())
            elif not entry.isdir():
                raise RuntimeError(f'Unsupported source archive member: {entry.name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--dist', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:[-.][A-Za-z0-9]+)*', args.version):
        raise ValueError('Invalid release version')
    if git('status', '--porcelain').strip():
        raise RuntimeError('Commit the release before archiving; working tree must be clean')
    dist = (ROOT / args.dist).resolve()
    for name in ['bin/ImageBatchProcessorStudio.exe', 'bin/ImageBatchProcessorCore.dll',
                 'bin/imgbp.exe', 'bin/shaders/process.spv', 'README.md']:
        if not (dist / name).is_file():
            raise FileNotFoundError(dist / name)
    releases = ROOT / 'releases'
    releases.mkdir(exist_ok=True)
    binary = releases / f'ImageBatchProcessor-{args.version}-win-x64.zip'
    source = releases / f'ImageBatchProcessor-{args.version}-source-with-dependencies.zip'
    checksums = releases / f'SHA256SUMS-{args.version}.txt'
    if any(p.exists() for p in [binary, source, checksums]):
        raise FileExistsError('Release asset already exists; choose a new version')
    manifest = {'version': args.version, 'commit': git('rev-parse', 'HEAD').decode().strip(),
                'files': {str(p.relative_to(dist)).replace('\\', '/'): digest(p)
                          for p in sorted(dist.rglob('*'))
                          if p.is_file() and p.name != 'manifest.json'}}
    (dist / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    with zipfile.ZipFile(binary, 'x', zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for path in sorted(dist.rglob('*')):
            if path.is_file():
                output.write(path, path.relative_to(dist))
    with zipfile.ZipFile(source, 'x', zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        archive_into(output, ROOT)
        for line in git('submodule', 'status').decode().splitlines():
            if line[0] != ' ':
                raise RuntimeError('Submodule must be initialized and match the pinned revision')
            _, path, *_ = line.split()
            archive_into(output, ROOT / path, path + '/')
    checksums.write_text(''.join(f'{digest(p)}  {p.name}\n' for p in [binary, source]), encoding='ascii')
    print(binary)
    print(source)
    print(checksums)


if __name__ == '__main__':
    main()

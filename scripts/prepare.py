#!/usr/bin/env python3
"""Fetch pinned open-source upstreams into a NEW directory and apply this overlay.
No SDK, firmware, IPSW or generated image is downloaded by this script.
"""
import argparse
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]

def run(args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--kernel-url', help='Optional local mirror for testing')
    parser.add_argument('--userland-url', help='Optional local mirror for testing')
    args = parser.parse_args()
    dest = args.destination.resolve()
    if dest.exists():
        parser.error('destination must not exist; existing workspaces are never overwritten')
    if sys.version_info < (3, 12):
        parser.error('Python 3.12+ is required for safe tar extraction')
    upstream = json.loads((ROOT / 'upstream.json').read_text())
    run(['git', 'clone', '--no-checkout', args.kernel_url or upstream['kernel']['url'], str(dest)])
    run(['git', 'checkout', '--detach', upstream['kernel']['commit']], dest)
    # The userland is a second git repository with the same work tree, as upstream
    # common/setup.sh expects. Preserve the kernel's root LICENSE/README/.gitignore.
    ugit = dest / '.userland.git'
    run(['git', 'clone', '--bare', args.userland_url or upstream['userland']['url'], str(ugit)])
    cmd = ['git', '--git-dir=' + str(ugit), '--work-tree=' + str(dest)]
    run(cmd + ['config', 'core.bare', 'false'])
    run(cmd + ['config', 'core.worktree', str(dest)])
    ref = upstream['userland']['commit']
    run(cmd + ['read-tree', ref])
    # Set HEAD to the pinned commit without modifying any upstream branches.
    run(cmd + ['update-ref', '--no-deref', 'HEAD', ref])
    archive = subprocess.check_output(cmd + ['archive', ref])
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        members = [m for m in tar.getmembers() if m.name not in {'LICENSE', 'README.md', '.gitignore'}]
        tar.extractall(dest, members=members, filter='data')
    run(cmd + ['update-index', '--skip-worktree', 'README.md', '.gitignore', 'LICENSE'])
    for patch in ['kernel.patch', 'userland.patch']:
        path = str(ROOT / 'patches' / patch)
        run(['git', 'apply', '--check', path], dest)
        run(['git', 'apply', path], dest)
    for source in (ROOT / 'overlay').rglob('*'):
        if source.is_file():
            target = dest / source.relative_to(ROOT / 'overlay')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    (dest / 'PORTING-OVERLAY.json').write_text(json.dumps(upstream, indent=2) + '\n')
    print(f'Prepared source workspace: {dest}')
    print('Next: follow upstream build prerequisites; xnu/apply.sh creates the patched XNU tree.')

if __name__ == '__main__':
    main()

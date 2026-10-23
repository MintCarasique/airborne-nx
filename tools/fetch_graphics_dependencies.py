"""Fetch the pinned Vulkan/shader dependencies without changing game inputs."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def git(folder,*arguments,capture=False):
    command=['git','-C',str(folder),*arguments]
    if capture:return subprocess.check_output(command)
    subprocess.run(command,check=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference',action='store_true',help='Also fetch the read-only NFS architecture reference')
    options=parser.parse_args()
    pins=json.loads((ROOT/'config/graphics_dependencies.json').read_text())
    folders={'mesa_switch':'mesa-switch','xenos_recomp':'XenosRecomp'}
    if options.reference:folders['nfsmw_reference']='reference-nfsmw'
    for name,folder in folders.items():
        pin=pins[name];destination=ROOT/'tools'/folder
        if not destination.exists():
            subprocess.run(['git','-c','core.autocrlf=false','clone','--depth','1','--filter=blob:none','--no-checkout',pin['url'],str(destination)],check=True)
            git(destination,'fetch','--depth','1','origin',pin['commit'])
            git(destination,'checkout','--detach',pin['commit'])
        elif git(destination,'rev-parse','HEAD',capture=True).decode().strip()!=pin['commit']:
            raise RuntimeError('Existing dependency has a different revision: '+folder)
        if name=='xenos_recomp':
            patch=ROOT/pin['patch']
            if hashlib.sha256(patch.read_bytes()).hexdigest()!=pin['patch_sha256']:
                raise RuntimeError('Airborne shader patch checksum mismatch')
            current=git(destination,'diff','--binary',capture=True)
            if not current:
                git(destination,'apply','--check',str(patch))
                git(destination,'apply',str(patch))
            elif hashlib.sha256(current).hexdigest()!=pin['patch_sha256']:
                raise RuntimeError('Refusing to overwrite local shader dependency edits')
            git(destination,'submodule','update','--init','--recursive','--depth','1')
        elif git(destination,'status','--porcelain',capture=True).strip():
            raise RuntimeError('Dependency contains local changes: '+folder)
        print(folder+': '+pin['commit'])


if __name__=='__main__':main()

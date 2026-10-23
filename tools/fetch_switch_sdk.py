"""Extract SDK files from a pinned official devkitPro container into the workspace.

No container is executed and no system packages are installed. Layer digests
are verified before WSL extracts the selected opt/devkitpro subtree.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import tarfile
import urllib.request

root=Path(__file__).resolve().parents[1]
cache=root/'tools/toolchain/nx-image';cache.mkdir(exist_ok=True)
repo='devkitpro/devkita64'
digest='sha256:82575ea78651b530b2e232bb3799cfd1fe331514e053d5f724bb4b28191fb79d'
token=json.load(urllib.request.urlopen('https://auth.docker.io/token?service=registry.docker.io&scope=repository:'+repo+':pull'))['token']
def fetch(url):
    return urllib.request.urlopen(urllib.request.Request(url,headers={'Authorization':'Bearer '+token,'Accept':'application/vnd.oci.image.manifest.v1+json'}),timeout=60)
with fetch('https://registry-1.docker.io/v2/'+repo+'/manifests/'+digest) as response:
    raw=response.read()
if hashlib.sha256(raw).hexdigest()!=digest.split(':')[1]:raise RuntimeError('Manifest digest mismatch')
manifest=json.loads(raw);(cache/'manifest.json').write_text(json.dumps(manifest,indent=2))
destination=root/'tools/toolchain/nx-sdk';destination.mkdir(exist_ok=True)
def linux(path):
    return subprocess.check_output(['wsl','--exec','wslpath','-a',str(path)],text=True).strip()
records=[]
for layer in manifest['layers'][-2:]:
    sha=layer['digest'].split(':')[1];archive=cache/(sha+'.tar.gz')
    if not archive.exists():
        print(f'Downloading official SDK layer: {layer["size"]//1024//1024} MiB',flush=True)
        with fetch('https://registry-1.docker.io/v2/'+repo+'/blobs/'+layer['digest']) as stream,archive.open('wb') as out:
            while data:=stream.read(1024*1024):out.write(data)
    with archive.open('rb') as stream:
        if hashlib.file_digest(stream,'sha256').hexdigest()!=sha:raise RuntimeError('Layer checksum mismatch')
    names=[]
    with tarfile.open(archive) as tar:
        for entry in tar:
            if entry.name.startswith('opt/devkitpro/') and '/.wh.' not in entry.name:
                if entry.name.startswith('opt/devkitpro/pacman/'):continue
                if '..' in Path(entry.name).parts:raise RuntimeError('Unsafe archive path')
                if entry.issym() or entry.islnk():
                    if entry.linkname.startswith('/') or '..' in Path(entry.linkname).parts:
                        # GCC uses relative links within its own toolchain; keep
                        # only paths resolving inside the SDK during extraction.
                        resolved=Path(entry.name).parent/entry.linkname
                        import posixpath
                        if not posixpath.normpath(str(resolved).replace('\\','/')).startswith('opt/devkitpro/'):
                            raise RuntimeError('SDK link escapes subtree: '+entry.name)
                names.append(entry.name)
    listing=cache/(sha+'.files');listing.write_text('\n'.join(names)+'\n',encoding='utf-8',newline='\n')
    if names:
        subprocess.run(['wsl','--exec','tar','-xzf',linux(archive),'--no-recursion','--no-same-owner','-C',linux(destination),'-T',linux(listing)],check=True)
    records.append(dict(digest=layer['digest'],bytes=layer['size'],sdk_entries=len(names)))
(root/'analysis/switch_sdk.json').write_text(json.dumps(dict(official_image=repo,image_manifest=digest,layers=records),indent=2))
print('SDK extracted within workspace: '+str(destination/'opt/devkitpro'),flush=True)

from pathlib import Path
import hashlib

def generated_fingerprint(folder: Path):
    digest=hashlib.sha256()
    for path in sorted(folder.glob('*')):
        if path.suffix not in ('.h','.cpp'):
            continue
        with path.open('rb') as stream:
            file_hash=hashlib.file_digest(stream,'sha256').hexdigest()
        digest.update((path.name+'\0'+file_hash+'\n').encode())
    return digest.hexdigest()

"""Validate the real translated shader corpus with WSL SPIRV-Tools."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]


def main():
    report_path=ROOT/'analysis/game_shader_translation.json'
    report=json.loads(report_path.read_text())
    if sys.platform=='win32':
        linux_root=subprocess.check_output(['wsl','--exec','wslpath','-a',str(ROOT)],text=True).strip()
        command=['wsl','--exec','python3',linux_root+'/tools/validate_game_spirv.py']
        raise SystemExit(subprocess.call(command))
    version=subprocess.check_output(['spirv-val','--version'],text=True).splitlines()[0]
    def validate(shader):
        path=ROOT/shader['path'].replace('\\','/')
        if hashlib.sha256(path.read_bytes()).hexdigest()!=shader['spirv']['sha256']:
            return dict(sha256=shader['sha256'],exit_code=-1,error='Artifact hash mismatch')
        result=subprocess.run(['spirv-val','--target-env','vulkan1.2',str(path)],capture_output=True,text=True)
        return dict(sha256=shader['sha256'],exit_code=result.returncode,error=result.stderr)
    with ThreadPoolExecutor(max_workers=6) as pool:
        results=list(pool.map(validate,[r for r in report['results'] if r['status']=='COMPILED']))
    errors=[r for r in results if r['exit_code']]
    output=dict(scope='SPIR-V structural/environment validation, not Xenos semantic equivalence or rendered output',
                validator=version,translation_sha256=hashlib.sha256(report_path.read_bytes()).hexdigest(),
                passed=len(results)-len(errors),failed=len(errors),first_error=errors[0] if errors else None,results=results)
    (ROOT/'analysis/game_spirv_validation.json').write_text(json.dumps(output,indent=2))
    print(f'SPIRV-Tools: {output["passed"]} accepted, {len(errors)} rejected')
    if errors:raise SystemExit(1)


if __name__=='__main__':main()

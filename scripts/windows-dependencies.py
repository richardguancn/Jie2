#!/usr/bin/env python3
"""Collect and audit the complete PE dependency graph, including Qt plugins."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

SYSTEM_DLLS = set('''kernel32.dll kernelbase.dll user32.dll gdi32.dll gdi32full.dll shell32.dll
advapi32.dll ole32.dll oleaut32.dll comdlg32.dll comctl32.dll ws2_32.dll shcore.dll
dwmapi.dll imm32.dll winmm.dll uxtheme.dll msimg32.dll d3d11.dll dxgi.dll d3d9.dll
d2d1.dll dwrite.dll userenv.dll setupapi.dll version.dll netapi32.dll wtsapi32.dll
iphlpapi.dll bcrypt.dll bcryptprimitives.dll ncrypt.dll secur32.dll mpr.dll crypt32.dll
wldap32.dll psapi.dll powrprof.dll shlwapi.dll winspool.drv mf.dll mfplat.dll
mfreadwrite.dll avrt.dll bluetoothapis.dll msvcrt.dll ucrtbase.dll ntdll.dll
rpcrt4.dll dnsapi.dll authz.dll dhcpcsvc.dll dwmapi.dll winhttp.dll wininet.dll
hid.dll cfgmgr32.dll opengl32.dll glu32.dll dcomp.dll propsys.dll urlmon.dll
normaliz.dll dbghelp.dll dbgcore.dll dxva2.dll cryptbase.dll usp10.dll
mswsock.dll pdh.dll winnsi.dll nsi.dll wintrust.dll imagehlp.dll runtimeobject.dll
windowscodecs.dll winusb.dll resampledmo.dll msdmo.dll strmiids.dll'''.split())

def is_system(name):
    return name.lower() in SYSTEM_DLLS or name.lower().startswith(('api-ms-win-', 'ext-ms-'))

def imports(binary, objdump):
    result = subprocess.run([objdump, '-p', str(binary)], check=True, capture_output=True, text=True)
    names = re.findall(r'DLL Name:\s*(\S+)', result.stdout, re.I)
    if not names:
        raise RuntimeError(f'无法读取导入表：{binary}')
    return names

def collect(package, runtime_dirs, scan, audit_only=False):
    package = Path(package)
    runtime = {}
    for folder in runtime_dirs:
        for file in sorted(Path(folder).glob('*')):
            if file.is_file() and file.suffix.lower() == '.dll':
                runtime.setdefault(file.name.lower(), file)
    # 根目录是 Windows 加载非系统依赖的公共搜索位置，插件子目录不算。
    bundled = {p.name.lower(): p for p in package.iterdir() if p.is_file()}
    queue = sorted(p for p in package.rglob('*') if p.suffix.lower() in ('.exe', '.dll'))
    seen, missing, copied = set(), [], []
    while queue:
        binary = queue.pop(0)
        if binary in seen: continue
        seen.add(binary)
        for name in scan(binary):
            lower = name.lower()
            if is_system(lower): continue
            if lower in bundled: continue
            source = runtime.get(lower)
            if source is None or audit_only:
                missing.append(f'{binary.relative_to(package)} -> {name}')
                continue
            target = package / source.name
            shutil.copy2(source, target)
            bundled[lower] = target
            queue.append(target)
            copied.append(source.name)
    if missing:
        raise RuntimeError('包内缺少依赖：\n  ' + '\n  '.join(sorted(set(missing))))
    return copied, len(seen)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('--runtime-dir', action='append', default=[])
    parser.add_argument('--objdump', default='objdump')
    parser.add_argument('--audit-only', action='store_true')
    args = parser.parse_args()
    copied, count = collect(args.package, args.runtime_dir,
                            lambda file: imports(file, args.objdump), args.audit_only)
    for name in copied: print('补入：' + name)
    print(f'依赖核对通过：检查 {count} 个程序和库，补入 {len(copied)} 个库')

if __name__ == '__main__':
    main()

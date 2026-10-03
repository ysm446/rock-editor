"""重いコマンド（ビルド・テスト・撮影）を、他の作業を邪魔しないように低い優先度と限られたコアで実行する。

    python tools/run_low.py [--cores N] [--priority idle|below] -- <コマンド> [引数...]

- 既定はコア数の 1/3（24 コアなら 8）だけに割り当て（CPU アフィニティ）、優先度は「通常以下（below）」。
  子プロセス（MSBuild → cl.exe、ctest → テスト）にも引き継がれる。
- 標準出力・標準エラーはそのまま流す。終了コードはコマンドのものを返す。
- 例:
    python tools/run_low.py -- cmake --build build --config Debug --target rock_editor_tests
    python tools/run_low.py -- build/bin/Debug/rock_editor_tests.exe --only RockScatter
"""
import argparse
import ctypes
import os
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cores', type=int, default=0, help='使うコア数（既定: 全体の 1/3、最低 2）')
    parser.add_argument('--priority', choices=['idle', 'below'], default='below')
    parser.add_argument('command', nargs=argparse.REMAINDER, help='-- の後にコマンド')
    args = parser.parse_args()
    command = args.command
    if command and command[0] == '--':
        command = command[1:]
    if not command:
        parser.print_help()
        return 2
    # 実行ファイルは絶対パスにする（Windows の CreateProcess は "build/bin/x.exe" のような相対パスを解決しない）。
    if os.path.isfile(command[0]):
        command[0] = os.path.abspath(command[0])
    else:
        import shutil
        resolved = shutil.which(command[0])
        if resolved:
            command[0] = resolved
    total = os.cpu_count() or 4
    cores = args.cores if args.cores > 0 else max(2, total // 3)
    cores = min(cores, total)
    creation = 0
    if os.name == 'nt':
        creation = subprocess.IDLE_PRIORITY_CLASS if args.priority == 'idle' else subprocess.BELOW_NORMAL_PRIORITY_CLASS
    env = dict(os.environ)
    # MSBuild の並列プロジェクト数と cl.exe の /MP もコア数に合わせる（アフィニティだけだと全コア分のスレッドが
    # 少ないコアを奪い合う）。
    env.setdefault('CL', f'/MP{cores}')
    env['UseMultiToolTask'] = 'true'
    env['MSBUILDNODECOUNT'] = str(cores)
    process = subprocess.Popen(command, creationflags=creation, env=env)
    if os.name == 'nt':
        # 下位の cores 個のコアだけに割り当てる。子プロセスにも引き継がれる。
        mask = (1 << cores) - 1
        handle = int(process._handle)  # noqa: SLF001 — Popen の Windows ハンドル
        ctypes.windll.kernel32.SetProcessAffinityMask(ctypes.c_void_p(handle), ctypes.c_size_t(mask))
    else:
        try:
            os.sched_setaffinity(process.pid, set(range(cores)))
        except Exception:  # noqa: BLE001
            pass
    try:
        return process.wait()
    except KeyboardInterrupt:
        process.kill()
        return 130


if __name__ == '__main__':
    sys.exit(main())

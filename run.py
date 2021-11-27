import shlex
import subprocess
import os
import re

# Build
build = True
configs = ['bimodal next_line bingo bingo bingo lru 1',
           'bimodal next_line bingo_new bingo bingo lru 1']

# Run
replace = True
warmup = 10
simulation = 10
# bandwidths = list(range(2, 33, 2))
bandwidths = [2]
trace_dir = ''
regex = ''

# Parallelizing
max_processes = 6


def check_processes(processes, end=False):
    if len(processes) < max_processes:
        return

    if not end:
        processes[0].wait()
        processes.pop(0)
    else:
        for process in processes:
            process.wait()


if __name__ == '__main__':
    if build:
        for config in configs:
            os.system('./build_champsim.sh {}'.format(config))

    processes = []
    files = ["server_001.champsimtrace.xz","server_003.champsimtrace.xz",\
        "server_013.champsimtrace.xz","server_017.champsimtrace.xz",\
        "server_021.champsimtrace.xz","server_022.champsimtrace.xz",
        "server_036.champsimtrace.xz"]
    for config in configs:
        executable = '{}core'.format(config.replace(' ', '-'))
        for bandwidth in bandwidths:
            for file in files:
                output_file = 'results/{}M_{}B/{}-{}.txt'.format(simulation,
                                                                 bandwidth,
                                                                 file,
                                                                 executable)
                if not os.path.isfile(output_file) or replace:
                    check_processes(processes)
                    print(output_file)
                    cli = './run_champsim.sh {} {} {} {} {}/{}'.format(executable,
                                                                       warmup,
                                                                       simulation,
                                                                       bandwidth,
                                                                       trace_dir,
                                                                       file)
                    args = shlex.split(cli)
                    processes.append(subprocess.Popen(args))
                    print(processes[-1].pid)

    check_processes(processes, end=True)

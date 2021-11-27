import os
import copy
import json

configs = ['bimodal next_line bingo bingo bingo lru 1',
            'bimodal next_line bingo_new bingo bingo lru 1']
simulation = 10

bandwidths = {}
bandwidths[configs[0]] = list(range(2, 33, 2))
bandwidths[configs[1]] = [2]
trace_dir = ''

caches = ['L1D', 'L1I', 'L2C', 'LLC']
basedict = {
    'IPC': {},
    'L1D': {
        'USEFUL': {},
        'USELESS': {},
        'MISSES': {},
        'ACCURACY': {},
        'COVERAGE': {},
        'MISS LATENCY': {}
    },
    'L1I': {
        'USEFUL': {},
        'USELESS': {},
        'MISSES': {},
        'ACCURACY': {},
        'COVERAGE': {},
        'MISS LATENCY': {}
    },
    'L2C': {
        'USEFUL': {},
        'USELESS': {},
        'MISSES': {},
        'ACCURACY': {},
        'COVERAGE': {},
        'MISS LATENCY': {}
    },
    'LLC': {
        'USEFUL': {},
        'USELESS': {},
        'MISSES': {},
        'ACCURACY': {},
        'COVERAGE': {},
        'MISS LATENCY': {}
    },
}


def extract(file, summary, bandwidth):
    with open(file, 'r') as f:
        cache_index = 0
        cache = caches[cache_index]
        misses = 0
        for line in f:
            content = line.split()
            if line.startswith('CPU 0 cumulative IPC'):
                summary['IPC'][bandwidth] = float(content[4])
                continue

            if line.startswith(f'{cache} LOAD      ACCESS'):
                misses += float(content[-1])
                continue

            if line.startswith(f'{cache} RFO       ACCESS'):
                misses += float(content[-1])
                continue

            if line.startswith(f'{cache} PREFETCH  REQUESTED'):
                useful = float(content[-3])
                useless = float(content[-1])

                try:
                    accuracy = useful / (useful + useless)
                except Exception:
                    accuracy = float("nan")

                try:
                    coverage = useful / (useful + misses)
                except Exception:
                    coverage = float("nan")

                summary[cache]['USEFUL'][bandwidth] = int(useful)
                summary[cache]['USELESS'][bandwidth] = int(useless)
                summary[cache]['MISSES'][bandwidth] = int(misses)
                summary[cache]['ACCURACY'][bandwidth] = accuracy
                summary[cache]['COVERAGE'][bandwidth] = coverage
                continue

            if line.startswith(f'{cache} AVERAGE MISS LATENCY'):
                try:
                    latency = float(content[4])
                except Exception:
                    coverage = float("nan")
                summary[cache]['MISS LATENCY'][bandwidth] = latency

                cache_index += 1
                if cache_index == 4:
                    break

                cache = caches[cache_index]
                misses = 0


if __name__ == '__main__':
    files = os.listdir('../../dpc3_traces/{}'.format(trace_dir))
    files.sort()
    for file in files:
        if not file.startswith("server"):
            continue
        json_file = 'summary/{}.json'.format(file)
        data = {}
        for config in configs:
            data[config] = copy.deepcopy(basedict)
            executable = '{}core'.format(config.replace(' ', '-'))
            for bandwidth in bandwidths[config]:
                data_file = 'results/{}M_{}B/{}-{}.txt'.format(simulation,
                                                               bandwidth,
                                                               file,
                                                               executable)
                extract(data_file, data[config], bandwidth*100)
        with open(json_file, 'w') as f:
            json.dump(data, f, indent=4)

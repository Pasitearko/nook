#!/usr/bin/env python3
"""Isolated, repeatable directory, SQLite query and native Wayland idle measurements."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--binary', default='build/nook')
parser.add_argument('--output', default='build/benchmarks')
parser.add_argument('--idle-seconds', type=int, default=60)
parser.add_argument('--index-rows', type=int, default=1_000_000)
args = parser.parse_args()
binary = str(Path(args.binary).resolve())
root = Path(args.output).resolve()
root.mkdir(parents=True, exist_ok=True)
# Keep the real desktop XDG paths visible to Fcitx and other toolkit plugins.
# Only nook preferences, index and preview cache belong in the benchmark profile.
environment = dict(os.environ, NOOK_PROFILE_DIR=str(root))
results = {'platform': 'native Wayland', 'directory': [], 'queries': []}

def run(arguments, graphical=False):
    env = dict(environment)
    env['QT_QPA_PLATFORM'] = 'wayland' if graphical else 'offscreen'
    completed = subprocess.run([binary]+arguments, env=env, capture_output=True, text=True, timeout=90, check=True)
    return json.loads(completed.stdout.strip().splitlines()[-1])

for count in (10_000, 100_000):
    directory = root/f'directory-{count}'
    directory.mkdir(exist_ok=True)
    for i in range(count):
        (directory/f'item-{i:06d}.txt').touch(exist_ok=True)
    measured = run(['--benchmark-directory', str(directory)])
    results['directory'].append(measured)
    print('Directory:', measured, flush=True)

run(['--benchmark-query', 'initialize'])
database = root/'data'/'index.sqlite'
if not database.exists():
    matches = list((root/'data').rglob('index.sqlite'))
    assert len(matches) == 1, matches
    database = matches[0]
connection = sqlite3.connect(database)
present = connection.execute('SELECT count(*) FROM entries').fetchone()[0]
if present != args.index_rows:
    connection.execute('DELETE FROM entries')
    for start in range(0, args.index_rows, 5000):
        records = []
        for i in range(start, min(args.index_rows, start+5000)):
            name = f'document-{i:07d}.txt'
            records.append((f'file:///synthetic-data/{name}', '/synthetic-data', name, name, name, name, 0, i, 1700000000, 'document', 1))
        connection.executemany('INSERT INTO entries(uri,root,name,name_fold,pinyin,initials,folder,size,mtime,icon,seen) VALUES(?,?,?,?,?,?,?,?,?,?,?)', records)
        connection.commit()
    connection.execute("INSERT INTO entries_fts(entries_fts) VALUES('optimize')")
    connection.commit()
results['syntheticIndexRows'] = connection.execute('SELECT count(*) FROM entries').fetchone()[0]
results['syntheticIndexBytes'] = database.stat().st_size
connection.close()
for query in ('document-0001234', 'document', '12'):
    measured = run(['--benchmark-query', query])
    results['queries'].append({'query': query, **measured})
    print('Query:', results['queries'][-1], flush=True)

# Use an unchanged tiny fixture; synthetic directory generation is finished
# before measuring CPU. No index scan, preview or user preferences are touched.
sample = root/'idle-directory'
sample.mkdir(exist_ok=True)
(sample/'sample.txt').write_text('Idle measurement fixture\n')
log = (root/'wayland.log').open('w')
env = dict(environment, QT_QPA_PLATFORM='wayland')
process = subprocess.Popen([binary, '--isolated', '--no-index', str(sample)], env=env, stdout=log, stderr=log)
try:
    time.sleep(4)
    if process.poll() is not None:
        raise RuntimeError('Native Wayland window exited; see wayland.log')
    def ticks():
        data = Path(f'/proc/{process.pid}/stat').read_text().rsplit(')', 1)[1].split()
        return int(data[11])+int(data[12])
    def pss():
        for line in Path(f'/proc/{process.pid}/smaps_rollup').read_text().splitlines():
            if line.startswith('Pss:'):
                return int(line.split()[1])
        raise RuntimeError('PSS unavailable')
    begin = time.monotonic()
    tick_start = ticks()
    memory = [pss()]
    while time.monotonic()-begin < args.idle_seconds:
        time.sleep(0.5)
        memory.append(pss())
    elapsed = time.monotonic()-begin
    cpu = (ticks()-tick_start)/os.sysconf('SC_CLK_TCK')/elapsed*100
    results['idle'] = {'seconds': round(elapsed, 2), 'cpuPercentOfOneLogicalCore': round(cpu, 4),
                       'pssMiB': round(sum(memory)/len(memory)/1024, 2), 'peakPssMiB': round(max(memory)/1024, 2),
                       'conditions': 'one native Wayland window; no scan/preview; tiny directory; effects enabled'}
    print('Idle:', results['idle'], flush=True)
finally:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill(); process.wait()
    log.close()
(root/'results.json').write_text(json.dumps(results, indent=2, ensure_ascii=False)+'\n')

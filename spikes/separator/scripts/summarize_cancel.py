"""Turn cancel.log (run_cancel.sh) + timing_51a.log (run_timing.sh) into the RESULTS.md cancel-latency table.

usage: run from $SAWBLADE_SEP_DATA (reads cancel.log and timing_51a.log in the cwd). One segment = median 70 s separation / 12.
"""
import re,statistics,json
cl=open('cancel.log').read().splitlines()
rows=[];cur=None
for l in cl:
    if l.startswith('== ') and 'separator_' in l and '--threads' in l:
        eng='onnx' if 'separator_onnx' in l else ('blas' if 'build-a-blas' in l else 'eigen')
        model='6s' if '6s' in l.split('--model')[1].split()[0] else '4s'
        t=int(re.search(r'--threads (\d)',l).group(1)); cur=[eng,model,t]
    elif l.startswith('CANCEL '):
        cur.append(float(re.search(r'stop_latency_s=([\d.]+)',l).group(1))); rows.append(cur)
runs={};label=None
for line in open('timing_51a.log').read().splitlines():
    m=re.match(r"== (\S+) m=(\S+) threads=(\d+) rep=(\d+)",line)
    if m: label=(m.group(1),m.group(2),int(m.group(3)));continue
    if line.startswith('RESULT') and label:
        b=line[7:]
        sep=json.loads(b)['sep_s'] if b.startswith('{') else float(re.search(r'sep_s=([\d.]+)',b).group(1))
        runs.setdefault(label,[]).append(sep)
key={'eigen':'eigen-v3','blas':'blas-v3','onnx':'onnx'}
print('| engine | model | threads | one segment of compute (s) | stop latency after the flag was set (s) | latency / segment |')
print('|---|---|---|---|---|---|')
name={'eigen':'(a) Eigen','blas':'(a) OpenBLAS','onnx':'(b) ORT'}
for eng,model,t,lat in sorted(rows,key=lambda r:(['eigen','blas','onnx'].index(r[0]),r[1],r[2])):
    s=statistics.median(runs[(key[eng],model,t)])/12
    print(f'| {name[eng]} | {model} | {t} | {s:.1f} | {lat:.1f} | {lat/s:.2f} |')

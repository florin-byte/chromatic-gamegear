#!/usr/bin/env python3
"""linksim matrix: games x {CWL2 (0.17.2 timing), A, A+B} x RTT, in parallel.

  python3 -I run_matrix.py OUTDIR [--games g1,g2] [--rtts 2,17,50,150] [--variants base,a,ab] [--jobs N]
          [--extra "--reorder --loss 5"] [--tag NAME]

ROMs and save states are read from the owner's local folders (never copied into
the repo). Each run writes OUTDIR/<game>_<variant>_<rtt><tag>.txt (+ PPMs) and one JSON
line to OUTDIR/results.jsonl.
"""
import json, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, 'linksim')
ROMS = os.environ.get('LINKSIM_ROMS', 'roms')
TS = os.environ.get('LINKSIM_TOOL_STATES', 'states')
LT = os.environ.get('LINKSIM_LINKTEST', 'linktest.gb')
MRT = os.path.expanduser('~/ChromaticCartBackups/u1_tetris_20261010_205736/MR_TETRIS.gbc')
STATES = os.environ.get('LINKSIM_STATES', '')   # MR Tetris / Tetris DX battle states (made by make_states.sh)
SCN = os.path.join(HERE, 'scenarios')

PLAY = '250:3550:20:4:left|right|a|down'
GAMES = {
    'linktest': dict(a=LT, sa='-', b=LT, sb='-', args=['--ms', '60000', '--keys-a', '300:a', '--keys-b', '280:b']),
    'mrtetris': dict(a=MRT, sa='{S}/mrt_bat_a', b=MRT, sb='{S}/mrt_bat_b',
                     args=['--ms', '60000', '--player-a', '1:' + PLAY, '--player-b', '2:' + PLAY]),
    'tetrisdx': dict(a=f'{ROMS}/Tetris DX (World).gbc', sa='{S}/tdx_vs_a', b=f'{ROMS}/Tetris DX (World).gbc', sb='{S}/tdx_vs_b',
                     args=['--ms', '60000', '--player-a', '3:' + PLAY, '--player-b', '4:' + PLAY]),
    'smbdx': dict(a=f'{ROMS}/Super Mario Bros. Deluxe (USA, Europe).gbc', sa=f'{TS}/m_menu',
                  b=f'{ROMS}/Super Mario Bros. Deluxe (USA, Europe).gbc', sb=f'{TS}/m_menu',
                  args=['--ms', '61000', '--script', f'{SCN}/smb_vs.scn']),
    'y_trade': dict(a=f'{ROMS}/Pokémon Yellow Version.gbc', sa=f'{TS}/y_cc', b=f'{ROMS}/Pokémon Yellow Version.gbc', sb=f'{TS}/y_cc',
                    args=['--script', f'{SCN}/y_trade.scn'], ms={2: 200000, 17: 260000, 50: 400000, 150: 900000}),
    'y_battle': dict(a=f'{ROMS}/Pokémon Yellow Version.gbc', sa=f'{TS}/y_cc', b=f'{ROMS}/Pokémon Yellow Version.gbc', sb=f'{TS}/y_cc',
                     args=['--script', f'{SCN}/y_battle.scn'], ms={2: 260000, 17: 300000, 50: 420000, 150: 900000}),
    'gs_trade': dict(a=f'{ROMS}/Pokémon Gold Version.gbc', sa=f'{TS}/g_cc', b=f'{ROMS}/Pokémon Silver Version.gbc', sb=f'{TS}/s_cc',
                     args=['--script', f'{SCN}/gs_trade.scn'], ms={2: 260000, 17: 320000, 50: 500000, 150: 1100000}),
    'gs_battle': dict(a=f'{ROMS}/Pokémon Gold Version.gbc', sa=f'{TS}/g_cc', b=f'{ROMS}/Pokémon Silver Version.gbc', sb=f'{TS}/s_cc',
                      args=['--script', f'{SCN}/gs_battle.scn'], ms={2: 320000, 17: 380000, 50: 560000, 150: 1200000}),
}
VARIANTS = {'base': ['--esp', 'base'], 'a': ['--esp', 'a'], 'ab': ['--esp', 'a', '--sa']}


def parse(txt):
    r = {'sides': []}
    for line in txt.splitlines():
        m = re.match(r'side (\w): frames (\d+)/(\d+) lost (-?\d+), stalled (\d+) ms \(frames >half stalled (\d+)\)', line)
        if m:
            r['sides'].append(dict(frames=int(m[2]), wall=int(m[3]), lost=int(m[4]), stalled_ms=int(m[5]), half=int(m[6])))
            continue
        s = r['sides'][-1] if r['sides'] else None
        m = re.search(r'port: exchanges (\d+) .*holds (\d+) .* p50 ([\d.]+) p90 ([\d.]+) max ([\d.]+) ms, no-exchange (\d+).*safety (\d+)', line)
        if m and s is not None:
            s.update(exch=int(m[1]), holds=int(m[2]), hold_p50=float(m[3]), hold_p90=float(m[4]), hold_max=float(m[5]),
                     noexch=int(m[6]), safety=int(m[7]))
        m = re.search(r'core: master_done (\d+) timeouts (\d+) slave_done (\d+) abandons (\d+)', line)
        if m and s is not None:
            s.update(abandons=int(m[4]), timeouts=int(m[2]))
        m = re.search(r'sa lent (\d+) taken (\d+) revokes (\d+)/(\d+).* used (\d+) .*mismatch (\d+) stray (\d+)', line)
        if m and s is not None:
            s.update(lent=int(m[1]), taken=int(m[2]), revokes=int(m[4]), used=int(m[5]), mismatch=int(m[6]), stray=int(m[7]))
        m = re.search(r'input: presses (\d+) seen (\d+) unseen (\d+), mean latency to first read ([\d.]+)', line)
        if m and s is not None:
            s.update(presses=int(m[1]), unseen=int(m[3]), input_lat=float(m[4]))
        m = re.match(r'check: logs (\w+), (\d+) exchanges mirrored', line)
        if m:
            r['mirror'] = m[1] == 'MIRROR'
            r['mirrored'] = int(m[2])
        m = re.match(r'script: (\d+) wait timeouts', line)
        if m:
            r['script_timeouts'] = int(m[1])
        m = re.match(r'mark (\S+) t=([\d.]+) s', line)
        if m:
            r.setdefault('marks', {})[m[1]] = float(m[2])
    return r


def run(job):
    game, var, rtt, out, extra, tag = job
    g = GAMES[game]
    name = f'{game}_{var}_{rtt}{tag}'
    shot = os.path.join(out, name)
    ms = g.get('ms', {}).get(rtt)
    args = [SIM, g['a'], g['sa'].format(S=STATES), g['b'], g['sb'].format(S=STATES)] + g['args'] + VARIANTS[var]
    args += ['--rtt', str(rtt), '--jitter', str(round(rtt * 0.2, 1)), '--loss', '1', '--seed', str(7 + rtt), '--shot', shot]
    if ms:
        args += ['--ms', str(ms)]
    args += extra
    p = subprocess.run(args, capture_output=True, text=True)
    open(shot + '.txt', 'w').write(p.stdout)
    r = parse(p.stdout)
    r.update(game=game, variant=var, rtt=rtt, tag=tag, exit=p.returncode)
    return r


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    opt = dict(games=','.join(GAMES), rtts='2,17,50,150', variants='base,a,ab', jobs='24', extra='', tag='')
    a = sys.argv[2:]
    for i in range(0, len(a), 2):
        opt[a[i].lstrip('-')] = a[i + 1]
    jobs = [(g, v, int(r), out, opt['extra'].split(), opt['tag']) for g in opt['games'].split(',')
            for v in opt['variants'].split(',') for r in opt['rtts'].split(',')]
    with ThreadPoolExecutor(int(opt['jobs'])) as ex, open(os.path.join(out, 'results.jsonl'), 'a') as f:
        for r in ex.map(run, jobs):
            f.write(json.dumps(r) + '\n')
            f.flush()
            s = r['sides']
            print(f"{r['game']:9} {r['variant']:4} {r['rtt']:4} mirror={r.get('mirror')} timeouts={r.get('script_timeouts', '-')} "
                  + ' | '.join(f"half {x.get('half')} hold {x.get('hold_p50')}/{x.get('hold_p90')} ab {x.get('abandons')} unseen {x.get('unseen')}" for x in s),
                  flush=True)


if __name__ == '__main__':
    main()

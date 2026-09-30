#!/usr/bin/python3

#----------------------------------------------------------------
#
#  4190.307 Operating Systems (Fall 2026)
#
#  Project #3: The EDF Scheduler
#
#  September 30, 2026
#
#  Jin-Soo Kim
#  Systems Software & Architecture Laboratory
#  Dept. of Computer Science and Engineering
#  Seoul National University
#
#----------------------------------------------------------------

import sys
import matplotlib.pyplot as plt

def draw(file, figfile):
    data = []
    stack = []

    pidset = set()
    lineno = 0
    with open(file) as f:
        for line in f:
            lineno += 1
            if line and line[0].isdigit():
                l = line.strip().split()
                time = int(l[0])
                pid = int(l[1])
                action = l[2]
                pidset.add(pid)

                if action == 'starts':
                    stack.append((time, pid))
                elif action == 'ends':
                    st, sp = stack.pop()
                    data.append((st, time, pid))
                else:
                    print(f'Unknown string "{action}" in line {lineno}')
                    sys.exit(1)

    pids = list(pidset)
    intervals = {}
    for start, end, pid in data:
        if start not in intervals:
            intervals[start] = []
        if end not in intervals:
            intervals[end] = []
        intervals[start].append((pid, 1))
        intervals[end].append((pid, -1))
    
    intervals = sorted(intervals.items())

    start = intervals[0][0]
    lastpid = (intervals[0][1])[0][0]
    active_pids = [lastpid]
    runtimes = []
    mintime = start

    for t, changes in intervals[1:]:
        if lastpid != -1:
            runtimes.append((start, t, lastpid))
        for pid, state in changes:
            if state == 1:
                active_pids.append(pid)
            elif state == -1:
                active_pids.remove(pid)
        if not active_pids:
            lastpid = -1
        else:
            lastpid = active_pids[-1]
        start = t
    
    maxtime = start
    pidrun = { p:[] for p in pids }  
    for start, end, pid in runtimes:
        pidrun[pid].append((start, end))
    
    fig, axs = plt.subplots(len(pids), 1, figsize=(10, len(pids)*1.5), sharex=False, squeeze=False)
    axs = axs.flatten();
    plt.subplots_adjust(left=0.1, right=0.9, top=0.8, bottom=0.2, wspace=0.5, hspace=0.3)
    ylabels = [ "PID "+str(x) for x in pids]
    cmap = plt.cm.tab20c
    colors = [cmap(i / len(pids)) for i in range(len(pids))]

    for i in range(len(pids)):
        pid = pids[i]
        axs[i].set_xlim(left=mintime//10, right=maxtime//10+2)
        axs[i].set_ylim(bottom=0, top=1.2)
        axs[i].set_xticks(list(range(mintime//10, maxtime//10+2, 5)))
        axs[i].tick_params(axis='x', which='minor', bottom=True, direction='out', length=2)
        axs[i].minorticks_on()
        axs[i].set_yticks([])
        axs[i].set_ylabel(ylabels[i])
        axs[i].spines['top'].set_visible(False)
        axs[i].spines['left'].set_visible(False)
        axs[i].spines['right'].set_visible(False)

        for start, end in pidrun[pid]:
            rstart = start / 10.0
            rend = end / 10.0
            axs[i].barh(0, rend-rstart, left=rstart, height=1, color=colors[i])
                    
    plt.xlabel('Ticks')
    plt.savefig(figfile, format='png')
    plt.close()


if __name__=="__main__":
    if(len(sys.argv) > 3):
        print("usage : ./graph.py xv6.log graph.png")
    data = sys.argv[1] if len(sys.argv) > 1 else 'xv6.log'
    fig = sys.argv[2] if len(sys.argv) > 2 else 'graph.png'
    draw(data, fig)
    print("graph saved in the '%s' file" % fig);

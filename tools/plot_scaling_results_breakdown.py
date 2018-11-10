#!/usr/bin/env python
#
# Usage:
#  python plot_scaling_results.py input-file1-ext input-file2-ext ...
#
# Description:
# Plots speed up, parallel efficiency and time to solution given a "timesteps" output file generated by SWIFT.
#
# Example:
# python plot_scaling_results.py _hreads_cosma_stdout.txt _threads_knl_stdout.txt
#
# The working directory should contain files 1_threads_cosma_stdout.txt - 64_threads_cosma_stdout.txt and 1_threads_knl_stdout.txt - 64_threads_knl_stdout.txt, i.e wall clock time for each run using a given number of threads

import sys
import glob
import re
import numpy as np
import matplotlib.pyplot as plt
import scipy.stats
import ntpath

params = {
    "axes.labelsize": 14,
    "axes.titlesize": 18,
    "font.size": 12,
    "legend.fontsize": 12,
    "xtick.labelsize": 14,
    "ytick.labelsize": 14,
    "text.usetex": True,
    "figure.subplot.left": 0.055,
    "figure.subplot.right": 0.98,
    "figure.subplot.bottom": 0.05,
    "figure.subplot.top": 0.95,
    "figure.subplot.wspace": 0.14,
    "figure.subplot.hspace": 0.12,
    "lines.markersize": 6,
    "lines.linewidth": 3.0,
    "text.latex.unicode": True,
}
plt.rcParams.update(params)
plt.rc("font", **{"family": "sans-serif", "sans-serif": ["Times"]})

version = []
branch = []
revision = []
hydro_scheme = []
hydro_kernel = []
hydro_neighbours = []
hydro_eta = []
threadList = []
hexcols = [
    "#332288",
    "#88CCEE",
    "#44AA99",
    "#117733",
    "#999933",
    "#DDCC77",
    "#CC6677",
    "#882255",
    "#AA4499",
    "#661100",
    "#6699CC",
    "#AA4466",
    "#4477AA",
]
linestyle = (
    hexcols[0],
    hexcols[1],
    hexcols[3],
    hexcols[5],
    hexcols[6],
    hexcols[8],
    hexcols[2],
    hexcols[4],
    hexcols[7],
    hexcols[9],
)
numTimesteps = 0
legendTitle = " "

inputFileNames = []

# Work out how many data series there are
if len(sys.argv) == 1:
    print("Please specify an input file in the arguments.")
    sys.exit()
else:
    for fileName in sys.argv[1:]:
        inputFileNames.append(fileName)
    numOfSeries = int(len(sys.argv) - 1)

# Get the names of the branch, Git revision, hydro scheme and hydro kernel
def parse_header(inputFile):
    with open(inputFile, "r") as f:
        found_end = False
        for line in f:
            if "Branch:" in line:
                s = line.split()
                line = s[2:]
                branch.append(" ".join(line))
            elif "Revision:" in line:
                s = line.split()
                revision.append(s[2])
            elif "Hydrodynamic scheme:" in line:
                line = line[2:-1]
                s = line.split()
                line = s[2:]
                hydro_scheme.append(" ".join(line))
            elif "Hydrodynamic kernel:" in line:
                line = line[2:-1]
                s = line.split()
                line = s[2:5]
                hydro_kernel.append(" ".join(line))
            elif "neighbours:" in line:
                s = line.split()
                hydro_neighbours.append(s[4])
            elif "Eta:" in line:
                s = line.split()
                hydro_eta.append(s[2])
    return


# Parse file and return total time taken, speed up and parallel efficiency
def parse_files():

    totalTime = []
    sumTotal = []
    speedUp = []
    parallelEff = []

    for i in range(0, numOfSeries):  # Loop over each data series

        # Get path to set of files
        path, name = ntpath.split(inputFileNames[i])

        # Get each file that starts with the cmd line arg
        file_list = glob.glob(inputFileNames[i] + "*")

        threadList.append([])

        # Remove path from file names
        for j in range(0, len(file_list)):
            p, filename = ntpath.split(file_list[j])
            file_list[j] = filename

        # Create a list of threads using the list of files
        for fileName in file_list:
            s = re.split(r"[_.]+", fileName)
            threadList[i].append(int(s[1]))

        # Re-add path once each file has been found
        if len(path) != 0:
            for j in range(0, len(file_list)):
                file_list[j] = path + "/" + file_list[j]

        # Sort the thread list in ascending order and save the indices
        sorted_indices = np.argsort(threadList[i])
        threadList[i].sort()

        # Sort the file list in ascending order acording to the thread number
        file_list = [file_list[j] for j in sorted_indices]

        parse_header(file_list[0])

        branch[i] = branch[i].replace("_", "\\_")

        # version.append("$\\textrm{%s}$"%str(branch[i]))# + " " + revision[i])# + "\n" + hydro_scheme[i] +
        #                   "\n" + hydro_kernel[i] + r", $N_{ngb}=%d$"%float(hydro_neighbours[i]) +
        #                   r", $\eta=%.3f$"%float(hydro_eta[i]))
        totalTime.append([])
        speedUp.append([])
        parallelEff.append([])

        # Loop over all files for a given series and load the times
        for j in range(0, len(file_list)):
            times = np.loadtxt(file_list[j], usecols=(9,))
            updates = np.loadtxt(file_list[j], usecols=(6,))
            totalTime[i].append(np.sum(times))

        sumTotal.append(np.sum(totalTime[i]))

    # Sort the total times in descending order
    sorted_indices = np.argsort(sumTotal)[::-1]

    totalTime = [totalTime[j] for j in sorted_indices]
    branchNew = [branch[j] for j in sorted_indices]

    for i in range(0, numOfSeries):
        version.append("$\\textrm{%s}$" % str(branchNew[i]))

    global numTimesteps
    numTimesteps = len(times)

    # Find speed-up and parallel efficiency
    for i in range(0, numOfSeries):
        for j in range(0, len(file_list)):
            speedUp[i].append(totalTime[i][0] / totalTime[i][j])
            parallelEff[i].append(speedUp[i][j] / threadList[i][j])

    return (totalTime, speedUp, parallelEff)


def print_results(totalTime, parallelEff, version):

    for i in range(0, numOfSeries):
        print(" ")
        print("------------------------------------")
        print(version[i])
        print("------------------------------------")
        print("Wall clock time for: {} time steps".format(numTimesteps))
        print("------------------------------------")

        for j in range(0, len(threadList[i])):
            print(str(threadList[i][j]) + " threads: {}".format(totalTime[i][j]))

        print(" ")
        print("------------------------------------")
        print("Parallel Efficiency for: {} time steps".format(numTimesteps))
        print("------------------------------------")

        for j in range(0, len(threadList[i])):
            print(str(threadList[i][j]) + " threads: {}".format(parallelEff[i][j]))

    return


# Returns a lighter/darker version of the colour
def color_variant(hex_color, brightness_offset=1):

    rgb_hex = [hex_color[x : x + 2] for x in [1, 3, 5]]
    new_rgb_int = [int(hex_value, 16) + brightness_offset for hex_value in rgb_hex]
    new_rgb_int = [
        min([255, max([0, i])]) for i in new_rgb_int
    ]  # make sure new values are between 0 and 255
    # hex() produces "0x88", we want just "88"

    return "#" + "".join([hex(i)[2:] for i in new_rgb_int])


def plot_results(totalTime, speedUp, parallelEff, numSeries):

    fig, axarr = plt.subplots(2, 2, figsize=(10, 10), frameon=True)
    speedUpPlot = axarr[0, 0]
    parallelEffPlot = axarr[0, 1]
    totalTimePlot = axarr[1, 0]
    emptyPlot = axarr[1, 1]

    # Plot speed up
    speedUpPlot.plot(threadList[0], threadList[0], linestyle="--", lw=1.5, color="0.2")
    for i in range(0, numSeries):
        speedUpPlot.plot(threadList[0], speedUp[i], linestyle[i], label=version[i])

    speedUpPlot.set_ylabel("${\\rm Speed\\textendash up}$", labelpad=0.0)
    speedUpPlot.set_xlabel("${\\rm Threads}$", labelpad=0.0)
    speedUpPlot.set_xlim([0.7, threadList[0][-1] + 1])
    speedUpPlot.set_ylim([0.7, threadList[0][-1] + 1])

    # Plot parallel efficiency
    parallelEffPlot.plot(
        [threadList[0][0], 10 ** np.floor(np.log10(threadList[0][-1]) + 1)],
        [1, 1],
        "k--",
        lw=1.5,
        color="0.2",
    )
    parallelEffPlot.plot(
        [threadList[0][0], 10 ** np.floor(np.log10(threadList[0][-1]) + 1)],
        [0.9, 0.9],
        "k--",
        lw=1.5,
        color="0.2",
    )
    parallelEffPlot.plot(
        [threadList[0][0], 10 ** np.floor(np.log10(threadList[0][-1]) + 1)],
        [0.75, 0.75],
        "k--",
        lw=1.5,
        color="0.2",
    )
    parallelEffPlot.plot(
        [threadList[0][0], 10 ** np.floor(np.log10(threadList[0][-1]) + 1)],
        [0.5, 0.5],
        "k--",
        lw=1.5,
        color="0.2",
    )
    for i in range(0, numSeries):
        parallelEffPlot.plot(threadList[0], parallelEff[i], linestyle[i])

    parallelEffPlot.set_xscale("log")
    parallelEffPlot.set_ylabel("${\\rm Parallel~efficiency}$", labelpad=0.0)
    parallelEffPlot.set_xlabel("${\\rm Threads}$", labelpad=0.0)
    parallelEffPlot.set_ylim([0, 1.1])
    parallelEffPlot.set_xlim([0.9, 10 ** (np.floor(np.log10(threadList[0][-1])) + 0.5)])

    # Plot time to solution
    for i in range(0, numSeries):
        for j in range(0, len(threadList[0])):
            totalTime[i][j] = totalTime[i][j] * threadList[i][j]
            if i > 1:
                totalTime[i][j] = totalTime[i][j] + totalTime[i - 1][j]
        totalTimePlot.plot(threadList[0], totalTime[i], linestyle[i], label=version[i])

        if i > 1:
            colour = color_variant(linestyle[i], 100)
            totalTimePlot.fill_between(
                threadList[0],
                np.array(totalTime[i]),
                np.array(totalTime[i - 1]),
                facecolor=colour,
            )
        elif i == 1:
            colour = color_variant(linestyle[i], 100)
            totalTimePlot.fill_between(threadList[0], totalTime[i], facecolor=colour)

    totalTimePlot.set_xscale("log")
    totalTimePlot.ticklabel_format(style="sci", axis="y", scilimits=(0, 0))
    totalTimePlot.set_xlabel("${\\rm Threads}$", labelpad=0.0)
    totalTimePlot.set_ylabel(
        "${\\rm Time~to~solution~x~No.~of~cores}~[{\\rm ms}]$", labelpad=0.0
    )
    totalTimePlot.set_xlim([0.9, 10 ** (np.floor(np.log10(threadList[0][-1])) + 0.5)])
    # totalTimePlot.set_ylim(y_min, y_max)

    totalTimePlot.legend(
        bbox_to_anchor=(1.21, 0.97),
        loc=2,
        borderaxespad=0.0,
        prop={"size": 12},
        frameon=False,
        title=legendTitle,
    )
    emptyPlot.axis("off")

    for i, txt in enumerate(threadList[0]):
        if (
            2 ** np.floor(np.log2(threadList[0][i])) == threadList[0][i]
        ):  # only powers of 2
            speedUpPlot.annotate(
                "$%s$" % txt,
                (threadList[0][i], speedUp[0][i]),
                (threadList[0][i], speedUp[0][i] + 0.3),
                color=hexcols[0],
            )
            parallelEffPlot.annotate(
                "$%s$" % txt,
                (threadList[0][i], parallelEff[0][i]),
                (threadList[0][i], parallelEff[0][i] + 0.02),
                color=hexcols[0],
            )
            totalTimePlot.annotate(
                "$%s$" % txt,
                (threadList[0][i], totalTime[0][i]),
                (threadList[0][i], totalTime[0][i] * 1.1),
                color=hexcols[0],
            )

    # fig.suptitle("Thread Speed Up, Parallel Efficiency and Time To Solution for {} Time Steps of Cosmo Volume\n Cmd Line: {}, Platform: {}".format(numTimesteps),cmdLine,platform))
    fig.suptitle(
        "${\\rm Speed\\textendash up,~parallel~efficiency~and~time~to~solution~x~no.~of~cores~for}~%d~{\\rm time\\textendash steps}$"
        % numTimesteps,
        fontsize=16,
    )

    return


# Calculate results
(totalTime, speedUp, parallelEff) = parse_files()

legendTitle = version[0]

plot_results(totalTime, speedUp, parallelEff, numOfSeries)

print_results(totalTime, parallelEff, version)

# And plot
plt.show()

# WAB-Rtree: Workload-Aware Spatial Query Processing on Processing-in-Memory Systems

This repository contains the implementation and experimental baselines for **WAB-Rtree**, a workload-aware R-tree range-query processing framework for commercial **UPMEM Processing-in-Memory (PIM)** systems.

WAB-Rtree extends our previous Broadcast-Partitioned R-tree (**BP-Rtree**) design with:

* **Workload-aware leaf-shard replication** to reduce load imbalance across DPUs.
* **Replica-aware query partitioning** to distribute queries among replicas without redundant processing.
* **Sparse result return** to reduce DPU-to-host communication and host-side aggregation overhead.
* Communication-aware R-tree placement that replicates compact upper-level metadata while partitioning the larger leaf level across DPUs.

The repository also contains the previous PIM R-tree implementations and conventional CPU and GPU baselines used in our evaluation.

## Repository Structure

```text
WAB_Rtree_IEEE_TPDS/
├── WAB_Rtree/              # Proposed workload-aware PIM R-tree
│   ├── common/             # Shared host/DPU definitions
│   ├── dpu/                # DPU kernel
│   ├── host/               # Host runtime, R-tree construction, placement,
│   │                       # query distribution, and result aggregation
│   └── tools/              # Analysis and profiling utilities
│
├── BP_Rtree(ISC)/          # Broadcast-Partitioned R-tree from our ISC work
│   ├── common/
│   ├── dpu/
│   ├── host/
│   └── Makefile
│
├── SP_Rtree/               # Subtree-Partitioned PIM R-tree
│   ├── common/
│   ├── dpu/
│   └── host/
│
├── CPU_baseline/           # Sequential and multithreaded CPU R-tree
│
└── GPU_Baseline/           # CUDA R-tree baseline
```

## Experimental Datasets

The TPDS experiments use four real-world spatial datasets and one synthetic dataset.

| Dataset   | Number of Objects | Source   |
| --------- | ----------------: | -------- |
| Sports    |            ~999 K | UCR-STAR |
| Lakes     |            ~8.4 M | UCR-STAR |
| Parks     |            ~9.9 M | UCR-STAR |
| Synthetic |             ~16 M | SPIDER   |
| Buildings |           ~57.4 M | UCR-STAR |

Each spatial object is represented by its minimum bounding rectangle:

```text
xmin, ymin, xmax, ymax
```

Coordinates are converted to **32-bit integers using fixed-precision scaling** before R-tree construction and query processing.

Query rectangles are sampled from object MBRs from the corresponding dataset so that the query workload follows the native spatial distribution of the data.

For Sports, Lakes, Parks, and Synthetic, the evaluated query workloads are approximately:

```text
1%, 5%, 10%, and 25% of the dataset size
```

For Buildings, the workloads are:

```text
0.25%, 1.25%, 2.5%, and 6.25%
```

The same query files are used for the PIM, CPU, and GPU implementations.

## Downloading the Data and Queries

The datasets and query files are **not included directly in this GitHub repository because of their size**.

They can be downloaded from Google Drive:

**Data and Query Files:**
[Google Drive link will be added here]

After downloading, organize the files as:

```text
Data/
├── Synthetic/
├── Sports/
├── Lakes/
├── Parks/
└── Buildings/

Query/
├── Synthetic/
├── Sports/
├── Lakes/
├── Parks/
└── Buildings/
```

The dataset and query paths used by the programs can be configured in the corresponding `selectDataDataset()` and `selectQueryDataset()` functions.

> **Note:** The Google Drive archive contains the exact datasets and query workloads used for the experiments reported in the paper.

## Requirements

### WAB-Rtree, BP-Rtree, and SP-Rtree

A system equipped with **UPMEM PIM DIMMs** is required.

The following software is needed:

```text
UPMEM SDK
GCC
GNU Make
Python 3       # optional, for analysis/plotting tools
```

The experiments in the paper were performed on a UPMEM system containing 20 PIM DIMMs. The machine nominally provides 2,560 DPUs; we use a maximum of **2,540 DPUs** because configurations above this value are unstable on our system.

The default WAB-Rtree configuration is:

```text
DPUs:                  2540
Tasklets per DPU:      11
R-tree leaf capacity:  128
Base-shard capacity:   256 leaf nodes
```

Replica-aware query distribution uses block-cyclic scheduling with:

```text
256 queries/block     for workloads < 2 million queries
2048 queries/block    for workloads >= 2 million queries
```

### CPU Baseline

The CPU implementation requires:

```text
GCC
POSIX Threads (pthread)
GNU Make
```

Build the CPU baseline with:

```bash
cd CPU_baseline
make
```

Run it with:

```bash
RTREE_THREADS=32 ./rtree_cpu_baseline
```

`RTREE_THREADS` controls the number of worker threads. The experiments reported in the paper use **32 threads**.

### GPU Baseline

The GPU implementation requires:

```text
NVIDIA GPU
CUDA Toolkit
NVCC
```

The GPU baseline implements the same R-tree range-query semantics as the CPU and PIM implementations.

## Building the PIM Implementations

For the UPMEM implementations, both a host binary and a DPU binary must be compiled using the UPMEM SDK.

For example, BP-Rtree can be built with:

```bash
cd "BP_Rtree(ISC)"
make NR_DPUS=2540 NR_TASKLETS=11
```

This generates:

```text
build/host
build/dpu
```

Run the program with:

```bash
./build/host
```

The program interactively asks for the dataset and query workload to execute.

Different supported DPU configurations can be compiled by changing `NR_DPUS`, for example:

```bash
make clean
make NR_DPUS=2048 NR_TASKLETS=11
```

The resource-sensitivity experiments use:

```text
512
1024
2048
2540 DPUs
```

and tasklet counts:

```text
2
4
8
11
16
```

## Running WAB-Rtree

After building WAB-Rtree, run:

```bash
./build/host
```

The host program performs the following steps:

1. Loads the selected spatial dataset.
2. Constructs the STR-packed R-tree.
3. Loads the selected query workload.
4. Forms bounded leaf shards.
5. Estimates query pressure on spatial regions.
6. Assigns base shards and workload-aware replicas to DPUs.
7. Transfers R-tree metadata, leaf shards, and queries to the DPUs.
8. Executes parallel R-tree range queries.
9. Retrieves sparse partial results.
10. Aggregates the partial results on the host.

Execution statistics and timing information are written to the program's `logs/` directory.

## WAB-Rtree Configuration Options

Several WAB-Rtree behaviors can be controlled through environment variables.

### Skip CPU Verification

By default, WAB-Rtree can execute a host-side R-tree search for result verification.

To skip this step:

```bash
SKIP_CPU_VERIFY=1 ./build/host
```

This is useful for large experiments where CPU verification would substantially increase experiment duration.

### Sparse vs. Dense Result Return

Sparse result return is the default:

```bash
RESULT_RETURN_MODE=sparse ./build/host
```

To execute the dense-result-return ablation:

```bash
RESULT_RETURN_MODE=dense ./build/host
```

### Replica Query Assignment

Block-cyclic query distribution is the default:

```bash
REPLICA_QUERY_POLICY=block ./build/host
```

The alternative contiguous range policy can be selected with:

```bash
REPLICA_QUERY_POLICY=range ./build/host
```

The block size can be explicitly overridden using:

```bash
REPLICA_QUERY_BLOCK_QUERIES=256 ./build/host
```

### Replica Allocation Policy

The workload estimator used for replica allocation can be selected using:

```bash
WORKLOAD_SCORE_POLICY=query ./build/host
```

Supported policies include:

```text
uniform
query
data
combined
```

The default WAB-Rtree policy is **query-pressure-based allocation**.

These options are primarily provided for reproducing the ablation experiments in the paper.

## Output and Timing

The PIM implementation reports both kernel-only and total execution time.

`T_PIM,kernel` represents cumulative DPU kernel execution time across all query batches.

`T_PIM,total` includes:

```text
workload placement and index distribution
host-to-DPU query transfer
DPU kernel execution
DPU-to-host result transfer
host-side result aggregation
```

One-time CPU R-tree construction is reported separately.

This distinction is important when comparing WAB-Rtree against the CPU and GPU baselines.

## Analysis Tools

`WAB_Rtree/tools/` contains utilities used for additional performance characterization.

For example:

```text
plot_load_imbalance.py
```

can be used to visualize the distribution of work across DPUs.

```text
stream_bandwidth.c
```

contains the memory-bandwidth microbenchmark used for platform characterization.

## Reproducibility Notes

The experimental results may vary slightly depending on:

* UPMEM SDK and runtime version
* Available number of healthy DPUs
* Host CPU and memory configuration
* DPU allocation
* Operating-system activity
* Compiler version and optimization settings

For the main experiments in the paper, we use:

```text
2540 DPUs
11 tasklets/DPU
leaf capacity = 128
base-shard capacity = 256
```

The exact input datasets and query files used for the reported experiments are provided separately through the Google Drive link above.

## Paper

This repository accompanies:

**WAB-Rtree: Workload-Aware Spatial Query Processing on Processing-in-Memory Systems**
Tasmia Jannat, Michael Gowanlock, and Satish Puri
*Manuscript submitted to IEEE Transactions on Parallel and Distributed Systems (TPDS).*

WAB-Rtree extends our earlier work:

**Parallel R-Tree-based Spatial Query Processing on a Commercial Processing-in-Memory System**
Tasmia Jannat, Michael Gowanlock, and Satish Puri
*ISC High Performance 2026 Research Paper Proceedings.*

## Citation

The citation for the WAB-Rtree TPDS paper will be updated after publication.

For the original PIM R-tree work, please cite:

```bibtex
@inproceedings{jannat2026parallel,
  title     = {Parallel R-Tree-based Spatial Query Processing on a Commercial Processing-in-Memory System},
  author    = {Jannat, Tasmia and Gowanlock, Michael and Puri, Satish},
  booktitle = {ISC High Performance 2026 Research Paper Proceedings},
  year      = {2026}
}
```

## Contact

For questions regarding the implementation or experiments, please contact:

**Tasmia Jannat**
Department of Computer Science
Missouri University of Science and Technology

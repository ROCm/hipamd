## Installation for use with AMD GPUs
This section describes installing for use with AMD GPUs. For use with NVIDIA
GPUs see [here](INSTALL.md#installation-for-use-with-nvidia-gpus).

### Prerequisites

-   Install mesa-common-dev
-   Either build or install [COMGR](https://github.com/RadeonOpenCompute/ROCm-CompilerSupport), [CLANG](https://github.com/RadeonOpenCompute/llvm-project) and [Device Library](https://github.com/RadeonOpenCompute/ROCm-Device-Libs)

### Getting the source code

```bash
git clone -b develop https://github.com/ROCm-Developer-Tools/hipamd.git
git clone -b develop https://github.com/ROCm-Developer-Tools/hip.git
git clone -b develop https://github.com/ROCm-Developer-Tools/ROCclr.git
git clone -b develop https://github.com/RadeonOpenCompute/ROCm-OpenCL-Runtime.git
```

### Set the environment variables

```bash
export HIPAMD_DIR="$(readlink -f hipamd)"
export HIP_DIR="$(readlink -f hip)"
export ROCclr_DIR="$(readlink -f ROCclr)"
export OPENCL_DIR="$(readlink -f ROCm-OpenCL-Runtime)"
```

### Build HIPAMD
Commands to build hipamd are as following,

```bash
cd "$HIPAMD_DIR"
mkdir -p build; cd build
cmake -DHIP_COMMON_DIR=$HIP_DIR -DAMD_OPENCL_PATH=$OPENCL_DIR -DROCCLR_PATH=$ROCCLR_DIR -DCMAKE_PREFIX_PATH="<ROCM_PATH>/" -DCMAKE_INSTALL_PREFIX=$PWD/install ..
make -j$(nproc)
sudo make install
```

Note,
HIP_COMMON_DIR looks for hip common ([HIP](https://github.com/ROCm-Developer-Tools/HIP/)) source codes.
By default, release version of hipamd is built.




## Installation for use with NVIDIA GPUs

This section describes installing for use with NVIDIA GPUs. For use with AMD
GPUs see [here](INSTALL.md#installation-for-use-with-amd-gpus).

### Prerequisites

- The NVIDIA CUDA toolkit with the `nvcc` compiler should be installed and in
  the `PATH` and `LD_LIBRARY_PATH`

### Installation

One needs both the `hip` and `hipamd` source code repositories. These need to
be made known to `hipamd` during the CMake configure phase. The key CMake variables
are `HIP_PLATFORM=nvidia` and `HIP_COMMON=<path to the clone of hip repo>`.

The following shell script illustrates a minimal CUDA backed HIP install from
sources.  The script will clone the sources, configure, and install HIP for use
with CUDA in the current directory.  You may want to modify `BUILD_DIR`,
`INSTALL_DIR`, and `BRANCH` to suit your needs.  NOTE: This script will only
set you up to use HIP with CUDA.

```bash
#!/bin/bash

set -exuo pipefail

# the directory where to compile and install
BUILD_DIR=`pwd`/hip_nvidia/build
INSTALL_DIR=`pwd`/hip_nvidia/install

# the branch to use. the two HIP source repos should be checked out to the
# same branch.
BRANCH=rocm-4.5.x

# make the directories for the sources, build, and install
mkdir -p ${BUILD_DIR}
mkdir -p ${INSTALL_DIR}

# download the hip sources
cd ${BUILD_DIR}

git clone -b "${BRANCH}" --single-branch \
    https://github.com/ROCm-Developer-Tools/hip.git

git clone -b "${BRANCH}" --single-branch \
    https://github.com/ROCm-Developer-Tools/hipamd.git

# create the build directory
mkdir -p ${BUILD_DIR}/build_${BRANCH}
cd ${BUILD_DIR}/build_${BRANCH}

# configure, make, make install
cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
    -DHIP_COMMON_DIR="${BUILD_DIR}/hip" \
    -DHIP_PLATFORM=nvidia \
    ../hipamd

make -j install

```

### Post install and use

In order for tools such as `hipify-perl` and `hipcc` to work, the following
environment variables should be propagated to the runtime environment.

```
export HIP_PATH=${INSTALL_DIR}
export PATH=${HIP_PATH}/bin/:$PATH
export HIP_PLATFORM=nvidia
```

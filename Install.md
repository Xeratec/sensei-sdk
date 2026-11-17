<!--
SPDX-FileCopyrightText: 2024 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# NRF SDK Installation
Follow the instructions in the video tutorial [nRF Connect for VS Code](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-VS-Code/Tutorials) or the written instruction about [Installing the nRF Connect SDK](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation/install_ncs.html).

Next make sure to install the required version of the toolchain and nRF SDK in VSCode. We recommend to use nRF Connect SDK version `v3.4.1` and the corresponding toolchain version `v3.4.1`. You can check out the [Installing additional SDK versions](https://docs.nordicsemi.com/bundle/nrf-connect-vscode/page/get_started/quick_setup.html#installing-additional-sdk-versions) for more information.

Programming uses `nrfutil device` (`nrfutil install device`) by default. The `nrfjprog` command line tools are archived by Nordic, but `west flash --runner nrfjprog` still works if you have them.


Table with tested versions:
| nRF Connect SDK Version | Toolchain Version | nrfjprog Version |
|------------------------|-------------------|-------------------|
| v3.4.1                 | v3.4.1            | not needed (nrfutil device) |

## Using the Terminal
In case you want to use the terminal instead of the VSCode extension, follow the instructions below.

Make sure to export `ZEPHYR_BASE` pointing to the correct nRF Connect SDK folder. For example

```bash
export ZEPHYR_BASE=/opt/nordic/ncs/v3.4.1/zephyr
```

Next we recommend to setup a mamba/conda environment with the following command:
```bash
mamba create -n nrf python=3.12
mamba activate nrf
mamba install cmake=3 ccache
```

Then install the required packages:
```bash
pip install -r $ZEPHYR_BASE/scripts/requirements.txt
```

# Install the GAP SDK

The GAP9 code (`GAP9/`, the GAP9 side of the examples) builds with **GAP SDK 5.22.3**, in a Docker image tagged
`gap9:v5.22.3-sensei`. Other SDK versions are not supported.

The SDK source is the branch `wiesep/sensei` of the gap-sdk (release-v5.22.3 plus three fixes for NNTool and the
AutoTiler). The exact commit is pinned in `firmware/west.yml` of the node repository (project `gap-sdk`, group
`gap9`, access to `pulp-platform/gap-sdk` needed):

```bash
west config manifest.group-filter -- +gap9
west update gap-sdk
```

To build the image, see [GAP9.md](https://github.com/Xeratec/Deeploy/blob/pr/gap9/GAP9.md) of Deeploy and
`firmware/scripts/docker/README.md` of the node repository (section "GAP9 image"). Build the SDK from the pinned
commit in the image:

```bash
cd $GAP_SDK_HOME
make clean
make all
```

## Debug probe (OpenOCD)

Install the packages in the container, then build OpenOCD and start it:

```bash
apt update
apt install -y autoconf automake bison build-essential curl device-tree-compiler doxygen flex git graphviz gtkwave libfdt-dev libftdi-dev libftdi1 libjpeg-dev libsamplerate-dev libsdl2-dev libsdl2-ttf-dev libsndfile1-dev libtool libusb-1.0-0-dev pkg-config python3-pip qtbase5-dev libqt5charts5-dev rsync sox wget qt6-base-dev libqt6serialport6-dev libgl1-mesa-dev telnet
make openocd.all
```

```bash
cd scripts
openocd -d0 -c "gdb_port 3333; telnet_port 6666; tcl_port disabled" -f "interface/ftdi/olimex-arm-usb-ocd-h.cfg" -f "tcl/gap9revb.tcl"
```

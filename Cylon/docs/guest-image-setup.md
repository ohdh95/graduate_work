# Guest Image Setup

This guide describes how to prepare an Ubuntu 22.04 guest image with a custom CXL-enabled kernel and ndctl for use with QEMU/FEMU.

## 1. Download Ubuntu Base Image

```bash
FILE=~/images/jammy-server-cloudimg-amd64.qcow2
if [[ ! -f "$FILE" ]]; then
    sudo wget https://cloud-images.ubuntu.com/jammy/current/jammy-server-cloudimg-amd64.img \
        -O jammy-server-cloudimg-amd64.qcow2
fi
```

## 2. Resize and Prepare the Image

Convert the image and expand it by 100G:

```bash
sudo qemu-img convert \
  -f qcow2 \
  -O qcow2 \
  jammy-server-cloudimg-amd64.qcow2 \
  ubuntu22.qcow2

sudo qemu-img resize ubuntu22.qcow2 +100G
sudo qemu-img info ubuntu22.qcow2
```

Set the root password:

```bash
sudo virt-customize -a ubuntu22.qcow2 --root-password password:[PASSWORD]
```

## 3. Boot Image for Initial Setup

Launch QEMU with port forwarding for SSH access:

```bash
sudo qemu-system-x86_64 \
  -m 8G \
  -smp 8 \
  -machine accel=kvm \
  -enable-kvm \
  -nographic \
  -net nic,model=e1000 \
  -net user,hostfwd=tcp::2222-:22 \
  -hda ubuntu22.qcow2
```

---

## 4. Inside the Guest Image

### 4.1 Resize Root Filesystem

Delete and recreate the partition to use the full disk:

```bash
# Delete original partition
echo -e "d\n1\nw" | sudo fdisk /dev/sda

# Create new partition with increased size
echo -e "n\n1\n\n\nN\nw" | sudo fdisk /dev/sda
```

### 4.2 Configure GRUB for Serial Console

Edit `/etc/default/grub`:

```
GRUB_CMDLINE_LINUX="ip=dhcp console=ttyS0,115200 console=tty console=ttyS0"
GRUB_TERMINAL=serial
GRUB_SERIAL_COMMAND="serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1"
```

Apply and reboot:

```bash
update-grub
reboot
```

### 4.3 Apply Filesystem Resize

After reboot, expand the filesystem to fill the new partition:

```bash
resize2fs /dev/sda1
reboot
```

---

## 5. Build and Install CXL Kernel (6.4.6)

### 5.1 Install Build Dependencies

```bash
sudo apt-get update
sudo apt-get install -y network-manager openssh-server net-tools
sudo apt-get install -y git build-essential libncurses5 libncurses5-dev \
    bin86 libssl-dev bison flex libelf-dev
```

### 5.2 Download and Extract Kernel Source

```bash
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.4.6.tar.xz
tar xvf linux-6.4.6.tar.xz
cd linux-6.4.6
```

### 5.3 Configure Kernel

Start with the menu-based config:

```bash
make menuconfig
```

Then apply CXL and related options:

```bash
# CXL subsystem
sudo scripts/config --enable CONFIG_CXL_BUS
sudo scripts/config --enable CONFIG_CXL_PCI
sudo scripts/config --enable CONFIG_CXL_ACPI
sudo scripts/config --enable CONFIG_CXL_PMEM
sudo scripts/config --enable CONFIG_CXL_MEM
sudo scripts/config --enable CONFIG_CXL_PORT
sudo scripts/config --enable CONFIG_CXL_SUSPEND
sudo scripts/config --enable CONFIG_CXL_REGION
sudo scripts/config --enable CONFIG_DEV_DAX_CXL
sudo scripts/config --enable CXL_REGION_INVALIDATION_TEST
sudo scripts/config --enable CONFIG_CXL_MEM_RAW_COMMANDS

# NVDIMM
scripts/config --enable CONFIG_LIBNVDIMM
scripts/config --enable CONFIG_NVDIMM_PFN
scripts/config --enable CONFIG_NVDIMM_DAX
scripts/config --enable CONFIG_NVDIMM_KEYS

# Memory and security
sudo scripts/config --disable CONFIG_MEMORY_HOTPLUG_DEFAULT_ONLINE
sudo scripts/config --disable SYSTEM_TRUSTED_KEYS
sudo scripts/config --disable SYSTEM_REVOCATION_KEYS
sudo scripts/config --enable CONFIG_STRICT_DEVMEM
sudo scripts/config --disable CONFIG_HYPERVISOR_GUEST
```

### 5.4 Build and Install

```bash
make -j16 && make modules_install && make install
update-grub
reboot now
```

---

## 6. Install ndctl

### 6.1 Install Dependencies

```bash
sudo apt install -y git gcc g++ autoconf automake asciidoc asciidoctor \
    bash-completion xmlto libtool pkg-config libglib2.0-0 libglib2.0-dev \
    libfabric1 libfabric-dev doxygen graphviz pandoc libncurses5 \
    libkmod2 libkmod-dev libudev-dev uuid-dev libjson-c-dev libkeyutils-dev \
    libiniparser1 libiniparser-dev meson numactl libtraceevent-dev \
    libtracefs-dev udev
```

### 6.2 Build and Install ndctl

```bash
git clone https://github.com/pmem/ndctl
cd ndctl
git checkout v77
meson setup build
meson compile -C build
meson install -C build
```


### 6.3 Check CXL device

```bash
cxl list
```
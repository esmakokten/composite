#!/bin/bash

if [ $# -lt 2 ]; then
  echo "Usage: $0 <.../cos.iso > <arch: [x86_64|i386]> [debug]"
  exit 1
fi 

num_sockets=1
num_cores=16
num_threads=1
vcpus=$[${num_sockets}*${num_cores}*${num_threads}]
mem_size=4096
kvm_flag=""

arch=$2
debug_flag=$3
nic_flag=$4

if [ ! -f $1 ]
then
	echo "Cannot find the image iso"
	exit 1
fi

case ${arch} in
	x86_64 )
	;;
	i386 )
	;;
	* )
	echo "Unsupported architecture"
	exit 1
esac

if [ -e "/dev/kvm" ] && [ -r "/dev/kvm" ] && [ -w "/dev/kvm" ]
then
	kvm_flag="-enable-kvm"
fi

# An "iommu" argument (in either the debug or nic position) switches the
# machine type to q35 and attaches an emulated VT-d unit.  q35 is required:
# the default "pc" machine has no IOMMU.  Interrupt remapping stays off --
# Phase 1 is DMA remapping only -- but kernel-irqchip=split is what QEMU
# documents for intel-iommu, so it is set unconditionally here.
machine_flag=""
iommu_opts=" -M q35,kernel-irqchip=split -device intel-iommu,intremap=off "

if [ "${debug_flag}" == "iommu" ]
then
	debug_flag=""
	machine_flag="${iommu_opts}"
fi

if [ "${nic_flag}" == "iommu" ]
then
	nic_flag=""
	machine_flag="${iommu_opts}"
fi

if [ "${debug_flag}" == "debug" ]
then
	debug_flag="-S"
	if [ "${nic_flag}" == "enable-nic" ]
	then
		nic_flag=" -netdev type=tap,id=net0,ifname=tap0,script=no,downscript=no -device e1000e,netdev=net0,mac=66:66:66:66:66:66 "
	fi
elif [ "${debug_flag}" == "enable-nic" ]
then
	debug_flag=""
	nic_flag=" -netdev type=tap,id=net0,ifname=tap0,script=no,downscript=no -device e1000e,netdev=net0,mac=66:66:66:66:66:66 "
fi

if [ "${arch}" == "x86_64" ]
then
	qemu-system-x86_64 ${kvm_flag} -cpu max ${machine_flag} -smp ${vcpus},cores=${num_cores},threads=${num_threads},sockets=${num_sockets} -m ${mem_size} -cdrom $1 -no-reboot -nographic -s ${debug_flag} -nic none ${nic_flag}
elif [ "${arch}" == "i386" ]
then
	qemu-system-i386 ${kvm_flag} -cpu max -smp ${vcpus},cores=${num_cores},threads=${num_threads},sockets=${num_sockets} -m ${mem_size} -cdrom $1 -no-reboot -nographic -s ${debug_flag} ${nic_flag}
else
	echo "Unsupported arch!"
fi

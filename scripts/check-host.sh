#!/usr/bin/env bash
# Read-only host survey for GPU passthrough. Changes nothing.
echo "== CPU =="; lscpu | grep -E 'Model name|Flags' | sed 's/Flags:.*\(svm\|vmx\).*/virtualisation: \1/'
echo "== Kernel cmdline =="; cat /proc/cmdline
echo "== IOMMU enabled? =="; [ -d /sys/kernel/iommu_groups ] && echo "groups: $(ls /sys/kernel/iommu_groups | wc -l)" || echo "no iommu_groups"
echo "== GPUs and their IOMMU groups =="
for d in /sys/bus/pci/devices/*; do
  c=$(cat "$d/class"); case $c in 0x03*|0x0403*)
    g=$(basename "$(readlink "$d/iommu_group" 2>/dev/null)")
    echo "$(basename "$d") group=${g:-none} $(lspci -nns "$(basename "$d")" | cut -d' ' -f2-)";;
  esac
done
echo "== Tools =="; for t in qemu-system-x86_64 virsh; do command -v $t >/dev/null && echo "$t: yes" || echo "$t: no"; done
echo "== Firmware =="; [ -d /sys/firmware/efi ] && echo UEFI || echo BIOS
